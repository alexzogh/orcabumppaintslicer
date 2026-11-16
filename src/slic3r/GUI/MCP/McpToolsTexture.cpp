#include "McpTools.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureDisplacement.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

struct NamedProjection { const char *name; TextureProjectionMethod value; };
const NamedProjection projections[] = {
    {"triplanar", TextureProjectionMethod::Triplanar},
    {"cylindrical", TextureProjectionMethod::Cylindrical},
    {"spherical", TextureProjectionMethod::Spherical},
    {"lscm", TextureProjectionMethod::LSCM},
    {"view_projected", TextureProjectionMethod::ViewProjected},
    {"model_uv", TextureProjectionMethod::ModelUV},
    {"outline_wrap", TextureProjectionMethod::OutlineWrap},
};

struct NamedBlend { const char *name; TextureBlendMode value; };
const NamedBlend blends[] = {
    {"add", TextureBlendMode::Add},         {"subtract", TextureBlendMode::Subtract},
    {"multiply", TextureBlendMode::Multiply}, {"divide", TextureBlendMode::Divide},
    {"override", TextureBlendMode::Override},
};

const char *projection_name(TextureProjectionMethod value)
{
    for (const NamedProjection &entry : projections)
        if (entry.value == value)
            return entry.name;
    return "triplanar";
}

const char *blend_name(TextureBlendMode value)
{
    for (const NamedBlend &entry : blends)
        if (entry.value == value)
            return entry.name;
    return "add";
}

std::string projection_choices()
{
    std::string out;
    for (const NamedProjection &entry : projections)
        out += (out.empty() ? "" : ", ") + std::string(entry.name);
    return out;
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

json layer_summary(const TextureDisplacementLayer &layer, int index)
{
    return json{{"slot", index},
                {"name", layer.name},
                {"depth_mm", layer.depth_mm},
                {"tiling_scale", layer.tiling_scale},
                {"rotation_deg", layer.rotation_deg},
                {"invert", layer.invert},
                {"midlevel", layer.midlevel},
                {"smoothing", layer.smoothing},
                {"tile_enabled", layer.tile_enabled},
                {"projection_method", projection_name(layer.projection_method)},
                {"blend_mode", blend_name(layer.blend_mode)},
                {"color_enabled", layer.color_enabled},
                {"has_image", !layer.empty()}};
}

// Applies whichever of the layer parameters the caller named, leaving the rest as they were. Shared
// by add and set so the two cannot drift in what they accept.
void apply_layer_args(TextureDisplacementLayer &layer, const json &args)
{
    if (args.contains("depth_mm"))
        layer.depth_mm = args.at("depth_mm").get<float>();
    if (args.contains("tiling_scale")) {
        const float scale = args.at("tiling_scale").get<float>();
        if (scale <= 0.f)
            throw ToolError("tiling_scale is the size of one texture tile in millimetres, so it must "
                            "be above zero.");
        layer.tiling_scale = scale;
    }
    if (args.contains("rotation_deg"))
        layer.rotation_deg = args.at("rotation_deg").get<float>();
    if (args.contains("invert"))
        layer.invert = args.at("invert").get<bool>();
    if (args.contains("midlevel"))
        layer.midlevel = args.at("midlevel").get<float>();
    if (args.contains("smoothing"))
        layer.smoothing = args.at("smoothing").get<float>();
    if (args.contains("tile_enabled"))
        layer.tile_enabled = args.at("tile_enabled").get<bool>();
    if (args.contains("color_enabled"))
        layer.color_enabled = args.at("color_enabled").get<bool>();

    if (args.contains("projection_method")) {
        const std::string wanted = args.at("projection_method").get<std::string>();
        const auto        found  = std::find_if(std::begin(projections), std::end(projections),
                                                [&wanted](const NamedProjection &e) { return wanted == e.name; });
        if (found == std::end(projections))
            throw ToolError("projection_method must be one of: " + projection_choices() + ".");
        layer.projection_method = found->value;
    }
    if (args.contains("blend_mode")) {
        const std::string wanted = args.at("blend_mode").get<std::string>();
        const auto        found  = std::find_if(std::begin(blends), std::end(blends),
                                                [&wanted](const NamedBlend &e) { return wanted == e.name; });
        if (found == std::end(blends))
            throw ToolError("blend_mode must be one of: add, subtract, multiply, divide, override.");
        layer.blend_mode = found->value;
    }
}

const json layer_properties = json{
    {"depth_mm", {{"type", "number"}, {"description", "How deep the texture cuts, along the surface normal."}}},
    {"tiling_scale", {{"type", "number"}, {"description", "Size of one texture tile, in millimetres."}}},
    {"rotation_deg", {{"type", "number"}}},
    {"invert", {{"type", "boolean"}}},
    {"midlevel", {{"type", "number"}}},
    {"smoothing", {{"type", "number"}}},
    {"tile_enabled", {{"type", "boolean"}}},
    {"color_enabled", {{"type", "boolean"},
                       {"description", "Also colour the surface from the image. Colour resolves per "
                                       "triangle, not per pixel, so fine detail needs a dense mesh."}}},
    {"projection_method", {{"type", "string"}}},
    {"blend_mode", {{"type", "string"}}}};

json merge(json a, const json &b)
{
    for (auto it = b.begin(); it != b.end(); ++it)
        a[it.key()] = it.value();
    return a;
}

} // namespace

void add_texture_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name         = "list_textures";
        tool.description  = "Lists the height-map textures available to apply, both the ones shipped with "
                            "the application and any the user has added. Use a name from here with "
                            "add_texture_layer.";
        tool.input_schema = schema(json{{"query", {{"type", "string"},
                                                   {"description", "Optional filter on the name."}}}});
        tool.run          = [on_gui](const json &args) -> json {
            const std::string query = args.value("query", std::string());
            json              found = json::array();
            on_gui([&] {
                for (const TextureLibraryEntry &entry : texture_library()) {
                    if (!query.empty() && entry.name.find(query) == std::string::npos)
                        continue;
                    const char *source = entry.source == TextureSource::Shipped ? "shipped" :
                                         (entry.source == TextureSource::User ? "user" : "plugin");
                    json item{{"name", entry.name}, {"source", source}};
                    if (!entry.pack_name.empty())
                        item["pack"] = entry.pack_name;
                    found.push_back(std::move(item));
                }
            });
            return json{{"textures", std::move(found)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "list_texture_layers";
        tool.description =
            "Lists the texture-displacement layers on a volume, with every parameter of each. A layer "
            "only cuts where its own mask is painted - use paint_region with channel \"texture\" and "
            "the slot number from here to say where.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"volume_id", {{"type", "integer"}}}},
                                   {"object_id"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            json                layers    = json::array();
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);
                for (size_t i = 0; i < volume->texture_displacement_layers.size(); ++i)
                    layers.push_back(layer_summary(volume->texture_displacement_layers[i], int(i)));
            });
            return json{{"layers", std::move(layers)},
                        {"max_layers", int(TEXTURE_DISPLACEMENT_MAX_LAYERS)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "add_texture_layer";
        tool.description =
            "Adds a texture-displacement layer to a volume and returns its slot. Name a texture from "
            "list_textures, or give a path to an image file. The layer displaces nothing until its "
            "mask is painted: follow this with paint_region, channel \"texture\", texture_slot set to "
            "the returned slot.";
        tool.input_schema = schema(merge(json{{"object_id", {{"type", "integer"}}},
                                              {"volume_id", {{"type", "integer"}}},
                                              {"texture", {{"type", "string"},
                                                           {"description", "A name from list_textures, or a file path."}}}},
                                        layer_properties),
                                   {"object_id", "texture"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const std::string   texture   = args.value("texture", std::string());
            if (texture.empty())
                throw ToolError("texture is required: a name from list_textures, or a path to an image.");

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);

                if (volume->texture_displacement_layers.size() >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
                    throw ToolError("This volume already has the maximum of " +
                                    std::to_string(int(TEXTURE_DISPLACEMENT_MAX_LAYERS)) +
                                    " texture layers. Remove one first.");

                // A library name is resolved to its file; anything else is taken as a path, so a
                // client that generated its own height map can hand it over directly.
                std::string path = texture;
                for (const TextureLibraryEntry &entry : texture_library())
                    if (entry.name == texture) {
                        path = entry.path;
                        break;
                    }

                std::string error;
                std::shared_ptr<std::vector<unsigned char>> image = load_texture_image_data(path, error);
                if (!image || image->empty())
                    throw ToolError("Could not read a height map from \"" + texture +
                                    "\": " + (error.empty() ? "unsupported or unreadable image" : error) +
                                    ". list_textures gives the names this build ships with.");

                TextureDisplacementLayer layer;
                layer.slot       = int(volume->texture_displacement_layers.size());
                layer.name       = texture;
                layer.path       = path;
                layer.image_data = std::move(image);
                apply_layer_args(layer, args);

                snapshot("Add texture layer (MCP)");
                volume->texture_displacement_layers.push_back(std::move(layer));
                notify_object_changed(object_id);

                const int slot = int(volume->texture_displacement_layers.size()) - 1;
                result         = layer_summary(volume->texture_displacement_layers[size_t(slot)], slot);
                result["note"] = "This layer displaces nothing until its mask is painted. Use "
                                 "paint_region with channel \"texture\" and texture_slot " +
                                 std::to_string(slot) + ".";
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name         = "set_texture_layer";
        tool.description  = "Changes parameters of an existing texture layer. Only the ones named are "
                            "changed; the rest stay as they were.";
        tool.input_schema = schema(merge(json{{"object_id", {{"type", "integer"}}},
                                              {"volume_id", {{"type", "integer"}}},
                                              {"slot", {{"type", "integer"}, {"minimum", 0}}}},
                                        layer_properties),
                                   {"object_id", "slot"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const int           slot      = args.value("slot", -1);

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);
                if (slot < 0 || size_t(slot) >= volume->texture_displacement_layers.size())
                    throw ToolError("This volume has " +
                                    std::to_string(volume->texture_displacement_layers.size()) +
                                    " texture layers, so slot " + std::to_string(slot) + " does not exist.");

                snapshot("Change texture layer (MCP)");
                TextureDisplacementLayer &layer = volume->texture_displacement_layers[size_t(slot)];
                apply_layer_args(layer, args);
                notify_object_changed(object_id);
                result = layer_summary(layer, slot);
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "remove_texture_layer";
        tool.description =
            "Removes a texture layer from a volume. The layers above it move down a slot, so their "
            "numbers change - and their paint masks do not follow them. Re-read list_texture_layers "
            "afterwards rather than reusing slot numbers from before.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"volume_id", {{"type", "integer"}}},
                                        {"slot", {{"type", "integer"}, {"minimum", 0}}}},
                                   {"object_id", "slot"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));
            const int           slot      = args.value("slot", -1);

            json result;
            on_gui([&] {
                ModelObject *object = require_object(object_id);
                ModelVolume *volume = require_volume(object, volume_id);
                if (slot < 0 || size_t(slot) >= volume->texture_displacement_layers.size())
                    throw ToolError("This volume has " +
                                    std::to_string(volume->texture_displacement_layers.size()) +
                                    " texture layers, so slot " + std::to_string(slot) + " does not exist.");

                snapshot("Remove texture layer (MCP)");
                const std::string name = volume->texture_displacement_layers[size_t(slot)].name;
                volume->texture_displacement_layers.erase(volume->texture_displacement_layers.begin() + slot);
                for (size_t i = 0; i < volume->texture_displacement_layers.size(); ++i)
                    volume->texture_displacement_layers[i].slot = int(i);
                notify_object_changed(object_id);

                result = json{{"removed_slot", slot},
                              {"name", name},
                              {"remaining", volume->texture_displacement_layers.size()}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
