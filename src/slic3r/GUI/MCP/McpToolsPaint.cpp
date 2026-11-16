#include "McpTools.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/ColorSlot.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// Which of a volume's paint channels a tool means. A volume carries two independent kinds: the
// filament each triangle prints in, and one mask per texture-displacement layer saying where that
// layer cuts. They are the same data structure over the same mesh, which is why one selector
// vocabulary serves both - but they mean entirely different things, so a tool must name which.
struct Channel
{
    int         texture_slot = -1; // -1 is the colour channel; 0..7 are texture layer masks
    std::string name         = "color";
};

Channel read_channel(const json &args)
{
    Channel     channel;
    const std::string kind = args.value("channel", std::string("color"));
    if (kind == "color" || kind == "colour") {
        channel.texture_slot = -1;
        channel.name         = "color";
        return channel;
    }
    if (kind == "texture") {
        const int slot = args.value("texture_slot", -1);
        if (slot < 0 || slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
            throw ToolError("channel \"texture\" needs texture_slot from 0 to " +
                            std::to_string(int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1) +
                            ", naming which texture layer's mask to paint.");
        channel.texture_slot = slot;
        channel.name         = "texture:" + std::to_string(slot);
        return channel;
    }
    throw ToolError("channel must be \"color\" (which filament a triangle prints in) or \"texture\" "
                    "(where one texture layer cuts, with texture_slot naming the layer).");
}

ColorRGB read_color(const json &value, const char *what)
{
    if (value.is_string()) {
        ColorRGB color;
        if (!decode_color(value.get<std::string>(), color))
            throw ToolError(std::string(what) + " is not a colour this build can read. Use \"#RRGGBB\".");
        return color;
    }
    if (value.is_array() && value.size() == 3) {
        double channels[3];
        for (int i = 0; i < 3; ++i) {
            if (!value[i].is_number())
                throw ToolError(std::string(what) + " must be three numbers, or a \"#RRGGBB\" string.");
            channels[i] = value[i].get<double>();
        }
        // 0..1 and 0..255 are both common, and a client will use whichever it has. Anything above 1
        // can only be the 0..255 form.
        const bool bytes = channels[0] > 1. || channels[1] > 1. || channels[2] > 1.;
        const double scale = bytes ? 255. : 1.;
        return ColorRGB(float(channels[0] / scale), float(channels[1] / scale), float(channels[2] / scale));
    }
    throw ToolError(std::string(what) + " must be a \"#RRGGBB\" string or three numbers.");
}

// Writes `state` onto `facets` of one channel.
//
// `replace` clears whatever the channel held first; otherwise the new paint merges over it, which is
// what makes painting a second region without erasing the first possible.
//
// set_facet works at whole-triangle granularity. A region the user refined by hand with a brush -
// sub-triangles within one facet - is coarsened to whole triangles wherever this writes, which the
// tool descriptions say out loud because a client cannot otherwise know it is destroying detail.
void write_facets(ModelVolume            *volume,
                  int                     texture_slot,
                  const std::vector<int> &facets,
                  EnforcerBlockerType     state,
                  bool                    replace)
{
    FacetsAnnotation &annotation = texture_slot < 0 ? volume->mmu_segmentation_facets :
                                                      volume->texture_displacement_facet(texture_slot);

    TriangleSelector selector(volume->mesh());
    if (!replace) {
        const TriangleSelector::TriangleSplittingData &existing = annotation.get_data();
        if (!existing.bitstream.empty())
            selector.deserialize(existing, false);
    }

    for (int facet : facets)
        selector.set_facet(facet, state);

    annotation.set(selector);
}

std::uint64_t require_object_id(const json &args)
{
    if (!args.contains("object_id") || !args.at("object_id").is_number_unsigned())
        throw ToolError("object_id is required, as the unsigned number list_objects reported.");
    return args.at("object_id").get<std::uint64_t>();
}

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

const json selector_schema = json{
    {"type", "object"},
    {"description",
     "Which triangles to act on. One of: {\"whole_volume\": true}; "
     "{\"plane\": {\"point\": [x,y,z], \"normal\": [x,y,z]}, \"side\": \"above\"|\"below\"}; "
     "{\"axis\": \"x\"|\"y\"|\"z\", \"from\": mm, \"to\": mm}; "
     "{\"facing\": {\"direction\": [x,y,z], \"max_angle_deg\": 45}}; "
     "{\"existing\": {\"state\": n}} for whatever is already painted. "
     "Add \"frame\": \"volume\"|\"object\"|\"world\" to say what the coordinates are relative to; "
     "the default is \"object\", the model as it sits on the plate."}};

const json channel_properties = json{
    {"channel", {{"type", "string"}, {"enum", {"color", "texture"}}}},
    {"texture_slot", {{"type", "integer"}, {"minimum", 0}}}};

json merge(json a, const json &b)
{
    for (auto it = b.begin(); it != b.end(); ++it)
        a[it.key()] = it.value();
    return a;
}

} // namespace

void add_paint_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name        = "paint_region";
        tool.description = "Paints part of a volume with a filament, or marks where a texture layer "
                           "cuts. Say which channel: \"color\" sets the filament a triangle prints in, "
                           "\"texture\" with texture_slot marks one layer's mask. The region is a "
                           "declarative selector rather than a brush stroke. Painting coarsens any "
                           "finer detail a brush left inside the triangles it touches.";
        tool.input_schema = schema(merge(json{{"object_id", {{"type", "integer"}}},
                                              {"volume_id", {{"type", "integer"}}},
                                              {"filament", {{"type", "integer"}, {"minimum", 1},
                                                            {"description", "1-based filament, for the color channel."}}},
                                              {"selector", selector_schema},
                                              {"replace", {{"type", "boolean"},
                                                           {"description", "Clear the channel first instead of merging over it."}}}},
                                        channel_properties),
                                   {"object_id", "selector"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const Channel       channel   = read_channel(args);
            const Selector      selector  = parse_selector(args.at("selector"));
            const bool          replace   = args.value("replace", false);

            EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;
            if (channel.texture_slot < 0) {
                const int filament = args.value("filament", 0);
                if (filament < 1 || filament > int(EnforcerBlockerType::ExtruderMax))
                    throw ToolError("filament is required for the color channel, from 1 to " +
                                    std::to_string(int(EnforcerBlockerType::ExtruderMax)) +
                                    ". Use set_region_color instead to name a colour rather than a slot.");
                state = EnforcerBlockerType(filament);
            }

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);

                const std::vector<std::uint8_t> existing = facet_states(volume, channel.texture_slot);
                const std::vector<int> facets = select_facets(selector, volume->mesh().its,
                                                              selector_frame(selector, object, volume), existing);
                if (facets.empty())
                    throw ToolError("That selector picked no triangles of this volume (" +
                                    describe(selector) + "). Nothing was painted.");

                snapshot("Paint region (MCP)");
                write_facets(volume, channel.texture_slot, facets, state, replace);
                notify_object_changed(object_id);

                result = json{{"painted_triangles", facets.size()},
                              {"of_total", volume->mesh().its.indices.size()},
                              {"channel", channel.name},
                              {"region", describe(selector)}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "set_region_color";
        tool.description =
            "Paints part of a volume a given colour, without the caller needing to know which "
            "filaments are loaded. The colour is decomposed into the loaded filaments and a mixed "
            "filament slot that prints it is found or created. Reachable colours are bounded by what "
            "is loaded, so the result reports achieved_color, which is what it will actually print "
            "as - check it, because a colour outside that range comes back visibly different. It "
            "also reports whether a new filament slot had to be created, since slots are finite.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"volume_id", {{"type", "integer"}}},
                                        {"color", {{"description", "\"#RRGGBB\", or [r,g,b]."}}},
                                        {"selector", selector_schema},
                                        {"replace", {{"type", "boolean"}}}},
                                   {"object_id", "color", "selector"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const ColorRGB      wanted    = read_color(args.at("color"), "color");
            const Selector      selector  = parse_selector(args.at("selector"));
            const bool          replace   = args.value("replace", false);

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);

                const std::vector<std::uint8_t> existing = facet_states(volume, -1);
                const std::vector<int> facets = select_facets(selector, volume->mesh().its,
                                                              selector_frame(selector, object, volume), existing);
                if (facets.empty())
                    throw ToolError("That selector picked no triangles of this volume (" +
                                    describe(selector) + "). Nothing was painted.");

                snapshot("Paint colour (MCP)");
                const ColorSlotResult slot = Slic3r::GUI::slot_for_color(wanted, -1);
                if (slot.slot < 0)
                    throw ToolError("No filament slot could be made for that colour. At least two "
                                    "physical filaments have to be loaded for a colour to be mixed, and "
                                    "the project may already be at its filament limit.");

                write_facets(volume, -1, facets, EnforcerBlockerType(slot.slot + 1), replace);
                notify_object_changed(object_id);

                result = json{{"painted_triangles", facets.size()},
                              {"of_total", volume->mesh().its.indices.size()},
                              {"filament_slot", slot.slot + 1},
                              {"achieved_color", slot.matched_color_hex},
                              {"requested_color", encode_color(wanted)},
                              {"created_new_slot", slot.created},
                              {"region", describe(selector)}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "apply_color_gradient";
        tool.description =
            "Ramps a colour gradient across a region by painting it in bands along an axis. Each band "
            "is solved as its own colour and costs a filament slot, so steps is capped and the result "
            "reports how many slots were created. Bands are cut along the axis across the selected "
            "region's own extent.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"volume_id", {{"type", "integer"}}},
                                        {"from_color", {{"description", "\"#RRGGBB\", or [r,g,b]."}}},
                                        {"to_color", {{"description", "\"#RRGGBB\", or [r,g,b]."}}},
                                        {"steps", {{"type", "integer"}, {"minimum", 2}, {"maximum", 16}}},
                                        {"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}}},
                                        {"selector", selector_schema}},
                                   {"object_id", "from_color", "to_color", "selector"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const ColorRGB      from      = read_color(args.at("from_color"), "from_color");
            const ColorRGB      to        = read_color(args.at("to_color"), "to_color");
            const Selector      selector  = parse_selector(args.at("selector"));
            const int           steps     = args.value("steps", 5);
            if (steps < 2 || steps > 16)
                throw ToolError("steps must be from 2 to 16. Each step spends a filament slot, and "
                                "slots are finite.");
            const std::string axis_name = args.value("axis", std::string("z"));
            const int         axis      = axis_name == "x" ? 0 : (axis_name == "y" ? 1 : (axis_name == "z" ? 2 : -1));
            if (axis < 0)
                throw ToolError("axis must be \"x\", \"y\" or \"z\".");

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);

                const std::vector<std::uint8_t> existing = facet_states(volume, -1);
                const Transform3d               frame    = selector_frame(selector, object, volume);
                const std::vector<int> facets = select_facets(selector, volume->mesh().its, frame, existing);
                if (facets.empty())
                    throw ToolError("That selector picked no triangles of this volume (" +
                                    describe(selector) + "). Nothing was painted.");

                // The gradient runs across what was actually selected, not the whole model, so
                // "ramp across the painted area" means what it sounds like.
                const auto &mesh = volume->mesh().its;
                double      low = 0., high = 0.;
                bool        first = true;
                std::vector<double> coordinate(facets.size(), 0.);
                for (size_t i = 0; i < facets.size(); ++i) {
                    const Vec3i32 &index = mesh.indices[size_t(facets[i])];
                    Vec3d          centre = Vec3d::Zero();
                    for (int corner = 0; corner < 3; ++corner)
                        centre += frame * mesh.vertices[size_t(index[corner])].cast<double>();
                    coordinate[i] = (centre / 3.)[axis];
                    low           = first ? coordinate[i] : std::min(low, coordinate[i]);
                    high          = first ? coordinate[i] : std::max(high, coordinate[i]);
                    first         = false;
                }
                const double span = high - low;

                snapshot("Colour gradient (MCP)");
                json bands    = json::array();
                int  created  = 0;
                for (int step = 0; step < steps; ++step) {
                    const double   t      = steps == 1 ? 0. : double(step) / double(steps - 1);
                    const ColorRGB wanted(float(from.r() + (to.r() - from.r()) * t),
                                          float(from.g() + (to.g() - from.g()) * t),
                                          float(from.b() + (to.b() - from.b()) * t));

                    std::vector<int> band;
                    for (size_t i = 0; i < facets.size(); ++i) {
                        const double at = span <= 0. ? 0. : (coordinate[i] - low) / span;
                        int          which = span <= 0. ? 0 : int(at * double(steps));
                        which              = std::min(which, steps - 1);
                        if (which == step)
                            band.push_back(facets[i]);
                    }
                    if (band.empty())
                        continue;

                    const ColorSlotResult slot = Slic3r::GUI::slot_for_color(wanted, -1);
                    if (slot.slot < 0)
                        throw ToolError("Ran out of filament slots partway through the gradient, after " +
                                        std::to_string(step) +
                                        " of " + std::to_string(steps) +
                                        " steps. The earlier steps were painted; undo to put it back.");
                    created += slot.created ? 1 : 0;
                    write_facets(volume, -1, band, EnforcerBlockerType(slot.slot + 1), false);
                    bands.push_back(json{{"step", step},
                                         {"triangles", band.size()},
                                         {"filament_slot", slot.slot + 1},
                                         {"achieved_color", slot.matched_color_hex},
                                         {"requested_color", encode_color(wanted)}});
                }
                notify_object_changed(object_id);

                result = json{{"bands", std::move(bands)},
                              {"axis", axis_name},
                              {"created_new_slots", created},
                              {"region", describe(selector)}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name         = "clear_paint";
        tool.description  = "Removes everything painted in one channel of a volume: either the filament "
                            "colouring, or one texture layer's mask. The texture layer itself is left in "
                            "place; only where it cuts is cleared.";
        tool.input_schema = schema(merge(json{{"object_id", {{"type", "integer"}}},
                                              {"volume_id", {{"type", "integer"}}}},
                                        channel_properties),
                                   {"object_id"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const Channel       channel   = read_channel(args);

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);
                snapshot("Clear paint (MCP)");
                if (channel.texture_slot < 0)
                    volume->mmu_segmentation_facets.reset();
                else
                    volume->texture_displacement_facet(channel.texture_slot).reset();
                notify_object_changed(object_id);
                result = json{{"cleared", channel.name}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name         = "get_paint_summary";
        tool.description  = "Reports what is painted on a volume: how many triangles carry each filament "
                            "in the colour channel, and how many are marked in each texture layer's mask. "
                            "Read this before painting over something, to see what is there.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"volume_id", {{"type", "integer"}}}},
                                   {"object_id"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);
                const size_t total  = volume->mesh().its.indices.size();

                json by_filament = json::object();
                for (std::uint8_t state : facet_states(volume, -1))
                    if (state != 0) {
                        const std::string key = std::to_string(int(state));
                        by_filament[key]      = by_filament.value(key, std::uint64_t(0)) + 1;
                    }

                json textures = json::array();
                for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
                    std::uint64_t painted = 0;
                    for (std::uint8_t state : facet_states(volume, slot))
                        painted += state != 0 ? 1 : 0;
                    if (painted > 0)
                        textures.push_back(json{{"texture_slot", slot}, {"painted_triangles", painted}});
                }

                result = json{{"object_id", object_id},
                              {"volume_id", std::uint64_t(volume->id().id)},
                              {"total_triangles", total},
                              {"color_by_filament", std::move(by_filament)},
                              {"texture_masks", std::move(textures)}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
