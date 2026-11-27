#include "McpProtocol.hpp"

#include <exception>
#include <utility>

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// JSON-RPC 2.0 reserved codes.
constexpr int code_parse_error      = -32700;
constexpr int code_invalid_request  = -32600;
constexpr int code_method_not_found = -32601;
constexpr int code_invalid_params   = -32602;
constexpr int code_internal_error   = -32603;

json error_response(const json &id, int code, const std::string &message)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json result_response(const json &id, json result)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// MCP returns a tool's output as a list of content blocks rather than a bare value.
json tool_content(const std::string &text, bool is_error)
{
    json out{{"content", json::array({json{{"type", "text"}, {"text", text}}})}};
    if (is_error)
        out["isError"] = true;
    return out;
}

} // namespace

void Registry::add(Tool tool)
{
    for (Tool &existing : m_tools)
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return;
        }
    m_tools.push_back(std::move(tool));
}

const Tool *Registry::find(const std::string &name) const
{
    for (const Tool &tool : m_tools)
        if (tool.name == name)
            return &tool;
    return nullptr;
}

json Registry::list() const
{
    json tools = json::array();
    for (const Tool &tool : m_tools)
        tools.push_back(json{{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.input_schema}});
    return json{{"tools", std::move(tools)}};
}

std::string handle_message(const std::string &text, const Registry &registry, const ServerInfo &info)
{
    json msg;
    try {
        msg = json::parse(text);
    } catch (const json::parse_error &) {
        // Nothing that did not parse can yield an id, so the spec has this reported against null.
        return error_response(nullptr, code_parse_error, "Parse error").dump();
    }

    if (!msg.is_object())
        return error_response(nullptr, code_invalid_request, "Request must be a JSON object").dump();

    // No id means a notification, which gets no reply at all - not even an error. Every client sends
    // notifications/initialized straight after the handshake, so answering one is not a theoretical
    // concern: it desynchronises the very first exchange.
    const bool is_notification = !msg.contains("id") || msg.at("id").is_null();
    const json id              = is_notification ? json(nullptr) : msg.at("id");

    const auto fail = [&id, is_notification](int code, const std::string &message) -> std::string {
        return is_notification ? std::string() : error_response(id, code, message).dump();
    };
    const auto respond = [&id, is_notification](json result) -> std::string {
        return is_notification ? std::string() : result_response(id, std::move(result)).dump();
    };

    if (!msg.contains("jsonrpc") || !msg["jsonrpc"].is_string() || msg["jsonrpc"].get<std::string>() != "2.0")
        return fail(code_invalid_request, "Only JSON-RPC 2.0 is supported");
    if (!msg.contains("method") || !msg["method"].is_string())
        return fail(code_invalid_request, "Missing method");

    const std::string method = msg["method"].get<std::string>();
    const json        params = msg.contains("params") ? msg["params"] : json::object();

    if (method == "initialize") {
        json result{
            {"protocolVersion", info.protocol_version},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", info.name}, {"version", info.version}}},
        };
        if (!info.instructions.empty())
            result["instructions"] = info.instructions;
        return respond(std::move(result));
    }

    if (method == "ping")
        return respond(json::object());

    if (method == "tools/list")
        return respond(registry.list());

    if (method == "tools/call") {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
            return fail(code_invalid_params, "tools/call requires a tool name");

        const std::string name = params["name"].get<std::string>();
        const Tool       *tool = registry.find(name);
        // Calling something that was never advertised is the client being wrong, not the tool
        // failing, so it is a protocol error rather than an isError result the model should try to
        // work around.
        if (tool == nullptr)
            return fail(code_invalid_params, "Unknown tool: " + name);
        if (!tool->run)
            return fail(code_internal_error, "Tool has no implementation: " + name);

        // A tool call with no id cannot report what it did - not success, not failure, not the
        // colour it actually achieved. JSON-RPC would have the server process it anyway, but these
        // tools mutate the user's project, and running one fire-and-forget is not a trade worth
        // making for a message no MCP client sends. Dropped rather than run.
        if (is_notification)
            return {};

        const json args = params.contains("arguments") ? params["arguments"] : json::object();
        try {
            json out = tool->run(args);
            // A tool may build its own content blocks (an image, several blocks); anything else is
            // wrapped as text here so tools do not each repeat the envelope.
            if (out.is_object() && out.contains("content"))
                return result_response(id, std::move(out)).dump();
            return result_response(id, tool_content(out.is_string() ? out.get<std::string>() : out.dump(2), false)).dump();
        } catch (const ToolError &e) {
            return result_response(id, tool_content(e.what(), true)).dump();
        } catch (const std::exception &e) {
            // Not a ToolError: the tool broke rather than failed, which is the server's problem and
            // not something the model can route around.
            return error_response(id, code_internal_error, e.what()).dump();
        }
    }

    return fail(code_method_not_found, "Unknown method: " + method);
}

}}} // namespace Slic3r::GUI::mcp
