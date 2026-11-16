#include "McpTools.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

Model *current_model()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        throw ToolError("No plater is open, so there is no project to work with.");
    return &plater->model();
}

} // namespace

ModelObject *require_object(std::uint64_t object_id)
{
    Model *model = current_model();
    for (ModelObject *object : model->objects)
        if (object != nullptr && std::uint64_t(object->id().id) == object_id)
            return object;
    throw ToolError("No object with id " + std::to_string(object_id) +
                    " is on the plate. Ids change when an object is removed and re-added, so call "
                    "list_objects again rather than reusing one from earlier in the conversation.");
}

ModelVolume *require_volume(ModelObject *object, std::uint64_t volume_id)
{
    if (object->volumes.empty())
        throw ToolError("Object " + std::to_string(object->id().id) + " has no volumes.");

    // A single-part model is the common case, and making the client look up a volume id just to
    // paint the only thing there is would be noise.
    if (volume_id == 0) {
        std::vector<ModelVolume *> printable;
        for (ModelVolume *volume : object->volumes)
            if (volume != nullptr && volume->is_model_part())
                printable.push_back(volume);
        if (printable.size() == 1)
            return printable.front();
        throw ToolError("Object " + std::to_string(object->id().id) + " has " +
                        std::to_string(printable.size()) +
                        " printable volumes, so volume_id is needed. list_objects reports them.");
    }

    for (ModelVolume *volume : object->volumes)
        if (volume != nullptr && std::uint64_t(volume->id().id) == volume_id)
            return volume;
    throw ToolError("Object " + std::to_string(object->id().id) + " has no volume with id " +
                    std::to_string(volume_id) + ".");
}

Transform3d selector_frame(const Selector &selector, const ModelObject *object, const ModelVolume *volume)
{
    switch (selector.frame) {
    case Selector::Frame::Volume:
        return Transform3d::Identity();
    case Selector::Frame::Object:
        return volume->get_matrix();
    case Selector::Frame::World:
        // Instance 0, because a selector describes a shape rather than a placement, and an object
        // with several instances has the same geometry in each.
        if (object->instances.empty())
            return volume->get_matrix();
        return object->instances.front()->get_matrix() * volume->get_matrix();
    }
    return volume->get_matrix();
}

json object_summary(const ModelObject *object)
{
    json volumes = json::array();
    for (const ModelVolume *volume : object->volumes) {
        if (volume == nullptr)
            continue;
        const auto &mesh = volume->mesh().its;
        volumes.push_back(json{{"id", std::uint64_t(volume->id().id)},
                               {"name", volume->name},
                               {"is_model_part", volume->is_model_part()},
                               {"triangles", mesh.indices.size()},
                               {"texture_layers", volume->texture_displacement_layers.size()},
                               {"has_color_paint", !volume->mmu_segmentation_facets.empty()}});
    }

    json instances = json::array();
    for (const ModelInstance *instance : object->instances) {
        if (instance == nullptr)
            continue;
        const Vec3d offset   = instance->get_offset();
        const Vec3d rotation = instance->get_rotation();
        const Vec3d scale    = instance->get_scaling_factor();
        instances.push_back(json{
            {"offset", {offset.x(), offset.y(), offset.z()}},
            {"rotation_deg",
             {rotation.x() * 180. / 3.14159265358979323846, rotation.y() * 180. / 3.14159265358979323846,
              rotation.z() * 180. / 3.14159265358979323846}},
            {"scale", {scale.x(), scale.y(), scale.z()}},
            {"printable", instance->printable}});
    }

    const BoundingBoxf3 box = object->bounding_box_exact();
    return json{{"id", std::uint64_t(object->id().id)},
                {"name", object->name},
                {"volumes", std::move(volumes)},
                {"instances", std::move(instances)},
                {"size_mm", {box.size().x(), box.size().y(), box.size().z()}}};
}

std::vector<std::uint8_t> facet_states(const ModelVolume *volume, int texture_slot)
{
    // The eight texture masks are named fields rather than an array, for access-rights reasons
    // Model.hpp explains; texture_displacement_facet() is the indexed accessor over them.
    const FacetsAnnotation &annotation = texture_slot < 0 ? volume->mmu_segmentation_facets :
                                                            volume->texture_displacement_facet(texture_slot);
    const TriangleSelector::TriangleSplittingData &data = annotation.get_data();
    if (data.bitstream.empty())
        return {};

    TriangleSelector selector(volume->mesh());
    selector.deserialize(data, false);

    const size_t              facets = volume->mesh().its.indices.size();
    std::vector<std::uint8_t> states(facets, 0);
    // get_facets_strict reports sub-triangles with the index of the original facet each came from,
    // which is what lets a per-facet view be rebuilt from a selection that was refined by a brush.
    for (int state = int(EnforcerBlockerType::ExtruderMax); state >= 1; --state) {
        std::vector<int>        source;
        const indexed_triangle_set painted = selector.get_facets_strict(EnforcerBlockerType(state), &source);
        for (int origin : source)
            if (origin >= 0 && size_t(origin) < facets)
                states[size_t(origin)] = std::uint8_t(state);
    }
    return states;
}

void snapshot(const std::string &name)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        plater->take_snapshot(name);
}

void notify_object_changed(std::uint64_t object_id)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    const Model &model = plater->model();
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i] != nullptr && std::uint64_t(model.objects[i]->id().id) == object_id) {
            plater->changed_object(int(i));
            break;
        }
    plater->schedule_background_process();
}

}}} // namespace Slic3r::GUI::mcp
