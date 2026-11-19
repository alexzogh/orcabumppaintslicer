#ifndef slic3r_McpTools_hpp_
#define slic3r_McpTools_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Point.hpp"

#include "McpProtocol.hpp"
#include "McpSelectors.hpp"

namespace Slic3r {
class ModelObject;
class ModelVolume;
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace mcp {

// Runs a piece of work on the GUI thread and waits for it, turning any failure into a ToolError.
// Every tool body goes through this, so each one reads as though it were simply running there.
using GuiCall = std::function<void(const std::function<void()> &)>;

// What a group of tools needs in order to register itself. Deliberately just the thread hop: a
// tool reaches the slicer through wxGetApp(), the same as the rest of the GUI.
struct ToolContext
{
    GuiCall on_gui;
};

// Each group lives in its own file so no single one grows into a catalogue nobody can read.
void add_project_tools(Registry &registry, const ToolContext &context);
void add_geometry_tools(Registry &registry, const ToolContext &context);
void add_config_tools(Registry &registry, const ToolContext &context);
void add_paint_tools(Registry &registry, const ToolContext &context);
void add_texture_tools(Registry &registry, const ToolContext &context);
void add_output_tools(Registry &registry, const ToolContext &context);
void add_app_tools(Registry &registry, const ToolContext &context);

// ---------------------------------------------------------------------------------------------
// Shared helpers. All of these touch the model, so all of them must be called on the GUI thread.
// ---------------------------------------------------------------------------------------------

// Finds an object by the id list_objects reported. Throws ToolError naming the id when it is gone,
// which is the common case after a client holds an id across a user's edit.
ModelObject *require_object(std::uint64_t object_id);

// Finds a volume within an object. When volume_id is 0 and the object has exactly one volume, that
// volume is used, so single-part models do not force the client to look one up first.
ModelVolume *require_volume(ModelObject *object, std::uint64_t volume_id);

// Maps a mesh vertex of `volume` into the frame `selector` is written in.
Transform3d selector_frame(const Selector &selector, const ModelObject *object, const ModelVolume *volume);

// The shape list_objects and friends report an object in.
nlohmann::json object_summary(const ModelObject *object);

// Reads the per-facet state of a paint channel as a plain vector indexed by facet, which is what
// the selectors work against. Empty when the channel holds nothing.
std::vector<std::uint8_t> facet_states(const ModelVolume *volume, int texture_slot);

// Takes an undo/redo snapshot, so every mutating tool is undoable by hand afterwards. Named for
// what the user will see in the undo list.
void snapshot(const std::string &name);

// Tells the GUI that an object changed enough to need re-rendering and re-slicing.
void notify_object_changed(std::uint64_t object_id);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpTools_hpp_
