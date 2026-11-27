#include "McpTools.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Model.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

constexpr double deg_per_rad = 180. / 3.14159265358979323846;
constexpr double rad_per_deg = 3.14159265358979323846 / 180.;

Plater *require_plater()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        throw ToolError("No plater is open, so there is no project to work with.");
    return plater;
}

std::uint64_t require_object_id(const json &args)
{
    if (!args.contains("object_id") || !args.at("object_id").is_number_unsigned())
        throw ToolError("object_id is required, as the unsigned number list_objects reported.");
    return args.at("object_id").get<std::uint64_t>();
}

Vec3d read_vec3(const json &value, const char *what)
{
    if (!value.is_array() || value.size() != 3)
        throw ToolError(std::string(what) + " must be an array of three numbers.");
    for (const json &component : value)
        if (!component.is_number())
            throw ToolError(std::string(what) + " must be an array of three numbers.");
    return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
}

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

const json number3 = json{{"type", "array"},
                          {"items", {{"type", "number"}}},
                          {"minItems", 3},
                          {"maxItems", 3}};

} // namespace

void add_project_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name        = "list_objects";
        tool.description = "Lists the objects on the current plate: id, name, overall size, the volumes "
                           "(parts) each is made of, and each instance's placement. Object and volume ids are "
                           "what every other tool takes; address things by id and never by position in "
                           "this list, because positions shift as soon as anything is removed.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            json objects = json::array();
            on_gui([&objects] {
                Plater *plater = require_plater();
                for (const ModelObject *object : plater->model().objects)
                    if (object != nullptr)
                        objects.push_back(object_summary(object));
            });
            return json{{"objects", std::move(objects)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name        = "import_model";
        tool.description = "Loads a model file onto the current plate and returns the objects it added. "
                           "Takes a path on the machine running OrcaSlicer. Any format the application "
                           "opens works: 3mf, stl, obj, step and so on. A 3mf may also carry print "
                           "settings, which are applied as they would be on a normal open.";
        tool.input_schema = schema(json{{"path", {{"type", "string"}, {"description", "Absolute path to the model file."}}}},
                                   {"path"});
        tool.run = [on_gui](const json &args) -> json {
            const std::string path = args.value("path", std::string());
            if (path.empty())
                throw ToolError("path is required.");

            json added = json::array();
            on_gui([&path, &added] {
                Plater *plater = require_plater();
                const boost::filesystem::path file(path);
                boost::system::error_code    ec;
                if (!boost::filesystem::exists(file, ec))
                    throw ToolError("There is no file at " + path + ".");

                const std::vector<size_t> indices = plater->load_files(std::vector<boost::filesystem::path>{file});
                const Model              &model   = plater->model();
                for (size_t index : indices)
                    if (index < model.objects.size() && model.objects[index] != nullptr)
                        added.push_back(object_summary(model.objects[index]));
            });

            if (added.empty())
                throw ToolError("Nothing was loaded from " + path +
                                ". The file may be empty, or in a format this build cannot read.");
            return json{{"added", std::move(added)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name         = "remove_object";
        tool.description  = "Removes an object from the plate. Undoable in the application, but the id is "
                            "gone afterwards and a new one is issued if the same model is loaded again.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}}}, {"object_id"});
        tool.run          = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            std::string         name;
            on_gui([object_id, &name] {
                Plater      *plater = require_plater();
                ModelObject *object = require_object(object_id);
                name                = object->name;
                const Model &model  = plater->model();
                for (size_t i = 0; i < model.objects.size(); ++i)
                    if (model.objects[i] == object) {
                        snapshot("Remove object (MCP)");
                        plater->remove(i);
                        break;
                    }
            });
            return json{{"removed", object_id}, {"name", name}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name        = "transform_object";
        tool.description = "Moves, rotates or scales an object. Every argument is optional, and the ones "
                           "given are applied together. position/rotation_deg/scale set an absolute value; "
                           "translate and rotate_deg add to what is there. Millimetres and degrees. "
                           "Rotation is XYZ Euler angles. By default this affects instance 0; pass "
                           "instance, or all_instances, to reach the others.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"position", number3},
                                        {"translate", number3},
                                        {"rotation_deg", number3},
                                        {"rotate_deg", number3},
                                        {"scale", number3},
                                        {"scale_uniform", {{"type", "number"}}},
                                        {"instance", {{"type", "integer"}}},
                                        {"all_instances", {{"type", "boolean"}}}},
                                   {"object_id"});
        tool.run = [on_gui](const json &args) -> json {
            const std::uint64_t object_id = require_object_id(args);
            json                result;
            on_gui([&args, object_id, &result] {
                ModelObject *object = require_object(object_id);
                if (object->instances.empty())
                    throw ToolError("Object " + std::to_string(object_id) + " has no instances to transform.");

                std::vector<ModelInstance *> targets;
                if (args.value("all_instances", false)) {
                    targets = object->instances;
                } else {
                    const int index = args.value("instance", 0);
                    if (index < 0 || size_t(index) >= object->instances.size())
                        throw ToolError("Object " + std::to_string(object_id) + " has " +
                                        std::to_string(object->instances.size()) + " instances, so instance " +
                                        std::to_string(index) + " does not exist.");
                    targets.push_back(object->instances[size_t(index)]);
                }

                snapshot("Transform object (MCP)");
                for (ModelInstance *instance : targets) {
                    if (args.contains("position"))
                        instance->set_offset(read_vec3(args.at("position"), "position"));
                    if (args.contains("translate"))
                        instance->set_offset(instance->get_offset() + read_vec3(args.at("translate"), "translate"));
                    if (args.contains("rotation_deg"))
                        instance->set_rotation(read_vec3(args.at("rotation_deg"), "rotation_deg") * rad_per_deg);
                    if (args.contains("rotate_deg"))
                        instance->set_rotation(instance->get_rotation() +
                                               read_vec3(args.at("rotate_deg"), "rotate_deg") * rad_per_deg);
                    if (args.contains("scale"))
                        instance->set_scaling_factor(read_vec3(args.at("scale"), "scale"));
                    if (args.contains("scale_uniform")) {
                        const double factor = args.at("scale_uniform").get<double>();
                        if (factor <= 0.)
                            throw ToolError("scale_uniform must be above zero.");
                        instance->set_scaling_factor(Vec3d(factor, factor, factor));
                    }
                }

                object->invalidate_bounding_box();
                notify_object_changed(object_id);
                result = object_summary(object);
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name        = "arrange_plate";
        tool.description = "Arranges every object on the plate so they fit and do not collide, the same as "
                           "the application's arrange command. This starts a background job and returns "
                           "immediately; call list_objects afterwards to read the placements it chose.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            on_gui([] {
                snapshot("Arrange (MCP)");
                require_plater()->arrange();
            });
            return json{{"started", true},
                        {"note", "Arrange runs in the background. Read list_objects once it has settled."}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name        = "orient_objects";
        tool.description = "Rotates objects to a better printing orientation, the same as the application's "
                           "auto-orient command. Starts a background job and returns immediately.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            on_gui([] {
                snapshot("Orient (MCP)");
                require_plater()->orient();
            });
            return json{{"started", true},
                        {"note", "Orient runs in the background. Read list_objects once it has settled."}};
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
