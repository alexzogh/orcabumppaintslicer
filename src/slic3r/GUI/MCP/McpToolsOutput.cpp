#include "McpTools.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

Plater *require_plater()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        throw ToolError("No plater is open, so there is nothing to slice.");
    return plater;
}

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

} // namespace

void add_output_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name = "slice";
        tool.description =
            "Slices the current plate. Slicing runs in the background and this returns as soon as it "
            "has started, so poll get_slice_report until it reports done - the figures are only "
            "meaningful once it is.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            on_gui([] {
                Plater *plater = require_plater();
                if (plater->model().objects.empty())
                    throw ToolError("The plate is empty, so there is nothing to slice.");
                plater->reslice();
            });
            return json{{"started", true},
                        {"note", "Poll get_slice_report; its \"slicing\" field says when this has finished."}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "get_slice_report";
        tool.description =
            "Reports the state of the current slice and, once it has finished, what it produced: "
            "estimated print time, filament used by length, volume, weight and cost, and the number "
            "of tool changes. Estimates are returned once the selected FFF plate has a valid "
            "result and the background process is idle.";
        tool.input_schema = schema(json::object());
        tool.run          = [on_gui](const json &) -> json {
            json result;
            on_gui([&result] {
                Plater *plater = require_plater();
                PartPlateList &plates = plater->get_partplate_list();
                PartPlate *plate = plates.get_plate(plates.get_curr_plate_index());
                const bool slicing = plater->is_background_process_slicing() ||
                                     plater->background_process().has_pending_work();

                const ModelObjectPtrs instances = plate ? plate->get_objects_on_this_plate() : ModelObjectPtrs{};
                const std::set<const ModelObject *> objects(instances.begin(), instances.end());
                result = json{{"slicing", slicing}, {"sliced", false}, {"objects_on_plate", objects.size()}};
                if (slicing) {
                    result["note"] = "Slicing is still running.";
                    return;
                }
                if (plater->printer_technology() != ptFFF) {
                    result["note"] = "Slice estimates are available for FFF printers.";
                    return;
                }

                const Print *print = plate ? plate->fff_print() : nullptr;
                if (print == nullptr || !plate->is_slice_result_valid() ||
                    print->print_statistics().estimated_normal_print_time.empty()) {
                    result["note"] = "The selected plate has no valid slice report. Call slice first.";
                    return;
                }

                const PrintStatistics &stats = print->print_statistics();
                result["sliced"]                      = true;
                result["estimated_print_time"]        = stats.estimated_normal_print_time;
                result["filament_used_mm"]            = stats.total_used_filament;
                result["extruded_volume_mm3"]         = stats.total_extruded_volume;
                result["weight_g"]                    = stats.total_weight;
                result["cost"]                        = stats.total_cost;
                result["tool_changes"]                = stats.total_toolchanges;
                result["filament_types"]              = stats.printing_filament_types;
                if (!stats.estimated_silent_print_time.empty())
                    result["estimated_print_time_silent"] = stats.estimated_silent_print_time;
            });
            return result;
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "export_gcode";
        tool.description =
            "Writes the sliced G-code to a path on the machine running OrcaSlicer, with no file "
            "dialog. The plate must be sliced first - call slice and wait for get_slice_report to "
            "report done. Writing happens in the background; an existing file at that path is "
            "overwritten, and that is not covered by undo.";
        tool.input_schema = schema(json{{"path", {{"type", "string"},
                                                  {"description", "Absolute path to write the G-code to."}}}},
                                   {"path"});
        tool.run = [on_gui](const json &args) -> json {
            const std::string path = args.value("path", std::string());
            if (path.empty())
                throw ToolError("path is required.");

            json result;
            on_gui([&] {
                Plater *plater = require_plater();
                if (plater->model().objects.empty())
                    throw ToolError("The plate is empty, so there is nothing to export.");
                if (plater->is_background_process_slicing())
                    throw ToolError("Slicing is still running. Wait for get_slice_report to report done.");
                if (plater->fff_print().print_statistics().estimated_normal_print_time.empty())
                    throw ToolError("The plate has not been sliced yet. Call slice first.");

                const boost::filesystem::path output(path);
                if (output.has_parent_path()) {
                    boost::system::error_code ec;
                    if (!boost::filesystem::exists(output.parent_path(), ec))
                        throw ToolError("The folder " + output.parent_path().string() + " does not exist.");
                }

                const bool existed = boost::filesystem::exists(output);
                if (!plater->export_gcode_to(output))
                    throw ToolError("The export could not be started. The plate may have failed to slice.");

                result = json{{"path", output.string()},
                              {"overwrote_existing", existed},
                              {"note", "Writing runs in the background; the file appears once it finishes."}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
