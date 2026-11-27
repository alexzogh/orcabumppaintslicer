#include "McpTools.hpp"

#include <string>
#include <utility>
#include <vector>

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/GUI_App.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

const char *level_name(AppActionRunResult::Level level)
{
    switch (level) {
    case AppActionRunResult::Level::Succeeded: return "success";
    case AppActionRunResult::Level::Error: return "error";
    case AppActionRunResult::Level::Busy: return "busy";
    case AppActionRunResult::Level::Info: break;
    }
    return "info";
}

} // namespace

void add_app_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name = "list_app_actions";
        tool.description =
            "Lists the application's own commands - everything the command palette offers - with the "
            "id each is run by. This is the escape hatch: anything the other tools do not cover, but "
            "the application itself can do, is likely in here. The list reflects live state, so it "
            "changes with the project.";
        tool.input_schema = schema(json{{"query", {{"type", "string"},
                                                   {"description", "Optional filter on id or title."}}},
                                        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}});
        tool.run = [on_gui](const json &args) -> json {
            const std::string query = args.value("query", std::string());
            const int         limit = args.value("limit", 100);

            json actions = json::array();
            on_gui([&] {
                // snapshot() recomputes the live-state entries (recent files, plates, configs) before
                // serialising, which is why the catalogue is read through it rather than by_id().
                const json all = wxGetApp().action_registry().snapshot();
                for (const json &action : all.is_array() ? all : json::array()) {
                    if (int(actions.size()) >= limit)
                        break;
                    const std::string id    = action.value("id", std::string());
                    const std::string title = action.value("title", std::string());
                    if (!query.empty() && id.find(query) == std::string::npos &&
                        title.find(query) == std::string::npos)
                        continue;
                    actions.push_back(action);
                }
            });
            return json{{"actions", std::move(actions)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "run_app_action";
        tool.description =
            "Runs one of the application's own commands by id, from list_app_actions. These are the "
            "same commands the user's menus and command palette run, so they do whatever they would "
            "do if a person clicked them - including commands that discard work. Read the id's title "
            "in list_app_actions before running it. A command that opens a dialog will wait for a "
            "person, and this will report that the interface is busy.";
        tool.input_schema = schema(json{{"id", {{"type", "string"}}},
                                        {"param", {{"type", "string"},
                                                   {"description", "Optional argument, for commands that take one."}}}},
                                   {"id"});
        tool.run = [on_gui](const json &args) -> json {
            const std::string id = args.value("id", std::string());
            if (id.empty())
                throw ToolError("id is required. list_app_actions gives the ids.");
            const std::string param = args.value("param", std::string());

            json result;
            on_gui([&] {
                ActionRegistry &actions = wxGetApp().action_registry();
                if (actions.by_id(id) == nullptr)
                    throw ToolError("No application command has the id \"" + id +
                                    "\". list_app_actions gives the ids that exist right now.");

                const AppActionRunResult ran = actions.run(id, param);
                result = json{{"id", id},
                              {"level", level_name(ran.level)},
                              {"message", ran.message.ToStdString()}};
            });
            return result;
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
