#include "McpTools.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// The three preset layers a setting can live in. A key belongs to exactly one of them, and the tab
// that owns it is also the thing that knows how to apply a change to it.
const Preset::Type preset_types[] = {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER};

const char *preset_label(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT: return "print";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER: return "printer";
    default: return "other";
    }
}

// Which tab owns a setting, or nullptr when no layer has it.
Tab *tab_owning(const std::string &key, Preset::Type *type_out = nullptr)
{
    for (Preset::Type type : preset_types) {
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr)
            continue;
        const DynamicPrintConfig *config = tab->get_config();
        if (config != nullptr && config->has(key)) {
            if (type_out != nullptr)
                *type_out = type;
            return tab;
        }
    }
    return nullptr;
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// What a client needs in order to use a setting: not just the value, but what it means, what it is
// measured in, and what it will accept. The config schema carries all of that and the GUI already
// shows it; without it a client is guessing at units and legal values.
json describe_option(const std::string &key, const ConfigOptionDef *def)
{
    json out{{"key", key}};
    if (def == nullptr)
        return out;

    if (!def->label.empty())
        out["label"] = def->label;
    if (!def->category.empty())
        out["category"] = def->category;
    if (!def->tooltip.empty())
        out["tooltip"] = def->tooltip;
    if (!def->sidetext.empty())
        out["units"] = def->sidetext;
    // The sentinels are float, not int: comparing against INT_MIN/INT_MAX is a lossy conversion that
    // clang-cl rejects outright, and would in any case never match.
    if (def->min > -FLT_MAX)
        out["min"] = def->min;
    if (def->max < FLT_MAX)
        out["max"] = def->max;
    if (!def->enum_values.empty())
        out["enum_values"] = def->enum_values;
    if (!def->enum_labels.empty())
        out["enum_labels"] = def->enum_labels;
    out["type"] = int(def->type);
    return out;
}

json schema(json properties, std::vector<std::string> required = {})
{
    json result{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        result["required"] = required;
    return result;
}

} // namespace

void add_config_tools(Registry &registry, const ToolContext &context)
{
    // By value: the lambdas outlive the ToolContext the caller passed in.
    const GuiCall on_gui = context.on_gui;

    {
        Tool tool;
        tool.name = "search_config_options";
        tool.description =
            "Searches the print settings by name, label or description, and returns the matching keys "
            "with what each one means, its units and its legal values. OrcaSlicer has well over a "
            "thousand settings, so start here rather than guessing a key: get_config and set_config "
            "both take the keys this returns.";
        tool.input_schema = schema(json{{"query", {{"type", "string"}}},
                                        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
                                   {"query"});
        tool.run = [on_gui](const json &args) -> json {
            const std::string query = lowered(args.value("query", std::string()));
            if (query.empty())
                throw ToolError("query is required.");
            const int limit = std::min(std::max(args.value("limit", 25), 1), 100);

            json found = json::array();
            on_gui([&] {
                // A caller types what the setting is called, not what its key looks like, so the
                // query is also tried with spaces as underscores: "layer height" then matches the
                // key layer_height exactly rather than merely appearing inside
                // independent_support_layer_height.
                std::string key_query = query;
                std::replace(key_query.begin(), key_query.end(), ' ', '_');

                // Ordered by how direct the match is, then by key length: among keys that all
                // contain the query, the shortest is almost always the setting itself and the
                // longer ones are its qualified relatives.
                std::vector<std::tuple<int, std::size_t, std::string>> ranked;
                for (const auto &entry : print_config_def.options) {
                    const std::string     &key  = entry.first;
                    const ConfigOptionDef &def  = entry.second;
                    const std::string      lkey = lowered(key);

                    int score = -1;
                    if (lkey == key_query)
                        score = 0;
                    else if (lkey.rfind(key_query, 0) == 0)
                        score = 1;
                    else if (lkey.find(key_query) != std::string::npos || lkey.find(query) != std::string::npos)
                        score = 2;
                    else if (lowered(def.label) == query)
                        score = 3;
                    else if (lowered(def.label).find(query) != std::string::npos)
                        score = 4;
                    else if (lowered(def.category).find(query) != std::string::npos)
                        score = 5;
                    else if (lowered(def.tooltip).find(query) != std::string::npos)
                        score = 6;

                    if (score >= 0)
                        ranked.emplace_back(score, lkey.size(), key);
                }
                std::sort(ranked.begin(), ranked.end());

                for (const auto &hit : ranked) {
                    if (int(found.size()) >= limit)
                        break;
                    const std::string &key = std::get<2>(hit);
                    json option = describe_option(key, print_config_def.get(key));
                    Preset::Type type = Preset::TYPE_INVALID;
                    if (tab_owning(key, &type) != nullptr)
                        option["preset"] = preset_label(type);
                    found.push_back(std::move(option));
                }
            });

            return json{{"options", std::move(found)},
                        {"note", "Settings layer in this order: printer, then print, then filament, then "
                                 "a per-object override. set_object_config writes the last of those."}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "get_config";
        tool.description =
            "Reads current print settings by key, with what each one means and what it will accept. "
            "Pass object_id as well to see that object's own overrides rather than the project-wide "
            "values. Use search_config_options to find keys.";
        tool.input_schema = schema(json{{"keys", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                        {"object_id", {{"type", "integer"}}}},
                                   {"keys"});
        tool.run = [on_gui](const json &args) -> json {
            if (!args.contains("keys") || !args.at("keys").is_array())
                throw ToolError("keys must be an array of setting keys. search_config_options finds them.");
            std::vector<std::string> keys;
            for (const json &key : args.at("keys"))
                if (key.is_string())
                    keys.push_back(key.get<std::string>());
            if (keys.empty())
                throw ToolError("keys was empty.");

            const bool          per_object = args.contains("object_id");
            const std::uint64_t object_id  = args.value("object_id", std::uint64_t(0));

            json values = json::array();
            on_gui([&] {
                const ModelObject *object = per_object ? require_object(object_id) : nullptr;

                for (const std::string &key : keys) {
                    json entry = describe_option(key, print_config_def.get(key));

                    Preset::Type type = Preset::TYPE_INVALID;
                    if (Tab *tab = tab_owning(key, &type); tab != nullptr) {
                        entry["preset"] = preset_label(type);
                        if (const ConfigOption *option = tab->get_config()->option(key); option != nullptr)
                            entry["value"] = option->serialize();
                    } else {
                        entry["error"] = "No print setting has this key. Use search_config_options.";
                    }

                    if (object != nullptr) {
                        const ModelConfigObject &config = object->config;
                        entry["object_override"] = config.has(key) ?
                                                       json(config.option(key)->serialize()) :
                                                       json(nullptr);
                    }
                    values.push_back(std::move(entry));
                }
            });
            return json{{"settings", std::move(values)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "set_config";
        tool.description =
            "Changes project-wide print settings. Takes {key: value} with values written the way the "
            "setting serialises - a number for a number, \"1\"/\"0\" for a switch, the enum's own name "
            "for a choice. The whole batch is validated before anything is written, so a bad key or "
            "value changes nothing. This edits the active presets, the same as typing in the settings "
            "tabs; it does not save a preset.";
        tool.input_schema = schema(json{{"settings", {{"type", "object"},
                                                      {"description", "Map of setting key to value."}}}},
                                   {"settings"});
        tool.run = [on_gui](const json &args) -> json {
            if (!args.contains("settings") || !args.at("settings").is_object() || args.at("settings").empty())
                throw ToolError("settings must be a non-empty object of {key: value}.");

            json applied = json::array();
            on_gui([&] {
                // Grouped by the preset layer that owns each key, so one call can change settings
                // that live in different tabs.
                std::vector<std::pair<Tab *, std::vector<std::pair<std::string, std::string>>>> batches;

                for (auto it = args.at("settings").begin(); it != args.at("settings").end(); ++it) {
                    const std::string key   = it.key();
                    const json       &value = it.value();
                    const std::string text  = value.is_string() ? value.get<std::string>() : value.dump();

                    Preset::Type type = Preset::TYPE_INVALID;
                    Tab         *tab  = tab_owning(key, &type);
                    if (tab == nullptr)
                        throw ToolError("No print setting has the key \"" + key +
                                        "\". Use search_config_options to find the right one; nothing was changed.");

                    auto batch = std::find_if(batches.begin(), batches.end(),
                                              [tab](const auto &entry) { return entry.first == tab; });
                    if (batch == batches.end()) {
                        batches.push_back({tab, {}});
                        batch = std::prev(batches.end());
                    }
                    batch->second.push_back({key, text});
                    applied.push_back(json{{"key", key}, {"value", text}, {"preset", preset_label(type)}});
                }

                snapshot("Change settings (MCP)");
                for (auto &batch : batches) {
                    try {
                        batch.first->apply_preset_config_values(batch.second);
                    } catch (const std::exception &e) {
                        // The tab validates and rolls its own batch back, so what reaches here is a
                        // readable reason rather than a half-applied preset.
                        throw ToolError(std::string("Setting rejected: ") + e.what());
                    }
                }
            });
            return json{{"applied", std::move(applied)}};
        };
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "set_object_config";
        tool.description =
            "Sets per-object setting overrides, which take precedence over the project-wide value for "
            "that object only. Pass null as a value to remove an override and fall back to the "
            "project. Use this rather than set_config when only one model should differ.";
        tool.input_schema = schema(json{{"object_id", {{"type", "integer"}}},
                                        {"settings", {{"type", "object"}}}},
                                   {"object_id", "settings"});
        tool.run = [on_gui](const json &args) -> json {
            if (!args.contains("object_id") || !args.at("object_id").is_number_unsigned())
                throw ToolError("object_id is required.");
            const std::uint64_t object_id = args.at("object_id").get<std::uint64_t>();
            if (!args.contains("settings") || !args.at("settings").is_object() || args.at("settings").empty())
                throw ToolError("settings must be a non-empty object of {key: value}.");

            json applied = json::array();
            on_gui([&] {
                ModelObject *object = require_object(object_id);

                // Checked before anything is written, so a bad key leaves the object alone.
                for (auto it = args.at("settings").begin(); it != args.at("settings").end(); ++it)
                    if (print_config_def.get(it.key()) == nullptr)
                        throw ToolError("No print setting has the key \"" + it.key() +
                                        "\". Use search_config_options; nothing was changed.");

                snapshot("Object settings (MCP)");
                for (auto it = args.at("settings").begin(); it != args.at("settings").end(); ++it) {
                    const std::string key = it.key();
                    if (it.value().is_null()) {
                        object->config.erase(key);
                        applied.push_back(json{{"key", key}, {"value", nullptr}});
                        continue;
                    }
                    const std::string text = it.value().is_string() ? it.value().get<std::string>() :
                                                                      it.value().dump();
                    // ModelConfig has no strict setter, so the value is parsed into a scratch config
                    // first: a bad one throws there, leaving the object untouched.
                    try {
                        DynamicPrintConfig scratch;
                        scratch.set_deserialize_strict(key, text);
                        const ConfigOption *parsed = scratch.option(key);
                        if (parsed == nullptr)
                            throw ToolError("Setting \"" + key + "\" could not be parsed.");
                        object->config.set_key_value(key, parsed->clone());
                    } catch (const ToolError &) {
                        throw;
                    } catch (const std::exception &e) {
                        throw ToolError("Setting \"" + key + "\" rejected: " + e.what());
                    }
                    applied.push_back(json{{"key", key}, {"value", text}});
                }
                notify_object_changed(object_id);
            });
            return json{{"object_id", object_id}, {"applied", std::move(applied)}};
        };
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::mcp
