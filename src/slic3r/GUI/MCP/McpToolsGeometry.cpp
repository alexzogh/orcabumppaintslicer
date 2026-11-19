#include "McpTools.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <slic3r/GUI/MCP/McpProtocol.hpp>
#include <string>
#include <utility>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "McpGeometry.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// A mesh this large, asked for whole, is a client that has not read what get_scene told it rather
// than a client that really wants forty megabytes of base64 over a wireless link.
constexpr std::size_t triangles_needing_a_budget = 2000000;

Plater *require_plater()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        throw ToolError("No plater is open, so there is no scene to read.");
    return plater;
}

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

// One drawable piece: a volume as placed by one instance.
//
// Holds a shared_ptr to the mesh rather than a reference to the volume, which is what lets the
// expensive half of the work happen after the GUI thread has been let go: the mesh stays alive
// even if the user deletes the object a moment later, and the picture a client gets is simply the
// one that was on the plate when it asked.
struct Node
{
    std::uint64_t                       object_id   = 0;
    std::string                         object_name;
    std::uint64_t                       instance_id = 0;
    bool                                printable   = true;
    std::uint64_t                       volume_id   = 0;
    std::string                         volume_name;
    bool                                is_model_part = true;
    Transform3d                         transform     = Transform3d::Identity();
    std::shared_ptr<const TriangleMesh> mesh;
};

} // namespace

void add_geometry_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name        = "get_scene";
        tool.description =
            "Describes the plate as something to draw: the printable area, and one node per volume "
            "per instance with the transform that places it and the key of the mesh it uses. Carries "
            "no mesh data, so it is cheap to call repeatedly; fetch the meshes themselves with "
            "get_mesh. A mesh key changes whenever that geometry changes, so a client that caches by "
            "key can poll this and re-fetch only what actually moved or was edited. Each transform is "
            "16 numbers in column-major order, mapping the mesh's own coordinates to plate "
            "coordinates. Millimetres throughout, Z up.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            std::vector<Node> nodes;
            json              bed;
            on_gui([&nodes, &bed] {
                Plater            *plater = require_plater();
                const BuildVolume &volume = plater->build_volume();
                bed = bed_json(volume.printable_area(), volume.printable_height(), std::string(volume.type_name()));

                for (const ModelObject *object : plater->model().objects) {
                    if (object == nullptr)
                        continue;
                    for (const ModelInstance *instance : object->instances) {
                        if (instance == nullptr)
                            continue;
                        for (const ModelVolume *model_volume : object->volumes) {
                            // A volume with no mesh behind it has nothing to draw, and nothing to
                            // key a cache on either, so it is left out of the scene rather than
                            // reported as an empty node the client would have to special-case.
                            if (model_volume == nullptr || !model_volume->get_mesh_shared_ptr())
                                continue;
                            Node node;
                            node.object_id     = std::uint64_t(object->id().id);
                            node.object_name   = object->name;
                            node.instance_id   = std::uint64_t(instance->id().id);
                            node.printable     = instance->printable;
                            node.volume_id     = std::uint64_t(model_volume->id().id);
                            node.volume_name   = model_volume->name;
                            node.is_model_part = model_volume->is_model_part();
                            node.transform     = instance->get_matrix() * model_volume->get_matrix();
                            node.mesh          = model_volume->get_mesh_shared_ptr();
                            nodes.push_back(std::move(node));
                        }
                    }
                }
            });

            // The GUI thread is free again. Fingerprinting every mesh is a pass over all of their
            // bytes, which is exactly the kind of work that must not happen up there.
            json out = json::array();
            for (const Node &node : nodes) {
                const indexed_triangle_set &mesh = node.mesh->its;
                out.push_back(json{{"object_id", node.object_id},
                                   {"object_name", node.object_name},
                                   {"instance_id", node.instance_id},
                                   {"printable", node.printable},
                                   {"volume_id", node.volume_id},
                                   {"volume_name", node.volume_name},
                                   {"is_model_part", node.is_model_part},
                                   {"transform", transform_json(node.transform)},
                                   {"mesh_key", mesh_key(node.volume_id, mesh)},
                                   {"triangles", mesh.indices.size()}});
            }
            return json{{"bed", std::move(bed)}, {"nodes", std::move(out)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name        = "get_mesh";
        tool.description =
            "Returns one volume's triangle mesh, in the volume's own coordinates - get_scene gives "
            "the transform that places it. Positions are float32 triples and indices int32 triples, "
            "base64 of the raw little-endian bytes. No normals are sent: compute them from the "
            "triangles, flat or smoothed as the renderer prefers. max_triangles decimates the mesh to "
            "fit a budget, which is how a tablet or a browser gets a usable preview of a model with "
            "millions of triangles; the result says whether it did and what the original count was. A "
            "decimated mesh is for looking at and never for measuring. mesh_key identifies the "
            "*source* mesh, so a client that varies max_triangles must include the budget in its own "
            "cache key.";
        tool.input_schema = schema(
            json{{"object_id", {{"type", "integer"}, {"description", "As list_objects or get_scene reported it."}}},
                 {"volume_id",
                  {{"type", "integer"},
                   {"description", "Omit, or 0, when the object has a single printable volume."}}},
                 {"max_triangles",
                  {{"type", "integer"},
                   {"minimum", 0},
                   {"description", "At most this many triangles; 0, the default, sends the mesh whole."}}}},
            {"object_id"});
        tool.run = [on_gui](const json &args) -> json {
            if (!args.contains("object_id") || !args.at("object_id").is_number_unsigned())
                throw ToolError("object_id is required, as the unsigned number list_objects or get_scene reported.");
            const std::uint64_t object_id = args.at("object_id").get<std::uint64_t>();
            const std::uint64_t volume_id = args.value("volume_id", std::uint64_t(0));

            std::size_t max_triangles = 0;
            if (args.contains("max_triangles")) {
                if (!args.at("max_triangles").is_number_unsigned())
                    throw ToolError("max_triangles must be a whole number of triangles, or 0 for no limit.");
                max_triangles = args.at("max_triangles").get<std::size_t>();
            }

            std::shared_ptr<const TriangleMesh> mesh;
            std::uint64_t                       resolved_volume_id = volume_id;
            std::string                         volume_name;
            on_gui([object_id, volume_id, &mesh, &resolved_volume_id, &volume_name] {
                ModelObject *object        = require_object(object_id);
                ModelVolume *model_volume  = require_volume(object, volume_id);
                resolved_volume_id         = std::uint64_t(model_volume->id().id);
                volume_name                = model_volume->name;
                mesh                       = model_volume->get_mesh_shared_ptr();
            });

            if (!mesh)
                throw ToolError("That volume has no mesh.");

            const indexed_triangle_set &source = mesh->its;
            if (max_triangles == 0 && source.indices.size() > triangles_needing_a_budget)
                throw ToolError("That volume has " + std::to_string(source.indices.size()) +
                                " triangles, which is too much to send whole. Pass max_triangles to get a "
                                "decimated mesh - a few hundred thousand is plenty to look at.");

            // Decimation and base64 both run here rather than inside on_gui, so a heavy mesh costs
            // the caller time and not the person using the application.
            const std::string key = mesh_key(resolved_volume_id, source);

            // Only copy the mesh when there is something to do to it: a budget that the mesh
            // already fits is the common case, and the meshes this guards against are the ones
            // where a pointless copy would be worth tens of megabytes.
            indexed_triangle_set        reduced;
            const bool                  over_budget = max_triangles != 0 && source.indices.size() > max_triangles;
            if (over_budget)
                reduced = simplify_to(source, max_triangles);
            const indexed_triangle_set &payload   = over_budget ? reduced : source;
            const bool                  decimated = payload.indices.size() != source.indices.size();

            json result = encode_mesh(payload);
            result["volume_id"]        = resolved_volume_id;
            result["volume_name"]      = volume_name;
            result["mesh_key"]         = key;
            result["simplified"]       = decimated;
            result["source_triangles"] = source.indices.size();
            return result;
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
