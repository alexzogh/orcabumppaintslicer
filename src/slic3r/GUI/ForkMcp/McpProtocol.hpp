#ifndef slic3r_McpProtocol_hpp_
#define slic3r_McpProtocol_hpp_

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace mcp {

// JSON-RPC 2.0 framing and the handful of MCP methods the server answers, with no transport and no
// slicer state anywhere in it.
//
// Kept separate from both for the same reason MainThreadDispatcher is kept separate from wxWidgets:
// the protocol has a lot of small rules that are easy to get subtly wrong - a notification must not
// be answered at all, a tool that fails is not a JSON-RPC error - and none of them need a socket or
// an event loop to exercise. The transport hands this a string and writes back whatever it returns.

// A failure *inside* a tool: the model asked for something sensible and it did not work (no object
// with that id, a colour outside the gamut, a bake that was cancelled).
//
// This is deliberately not a JSON-RPC error. Under MCP a tool failure comes back as an ordinary
// successful result carrying `isError: true`, so the model sees it as a turn it can react to and
// retry differently. JSON-RPC errors are for the client being broken - bad JSON, an unknown method -
// which the model cannot do anything useful about.
struct ToolError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// One tool the server exposes.
struct Tool
{
    std::string    name;
    std::string    description;
    // JSON Schema for the arguments object, as tools/list reports it.
    nlohmann::json input_schema = nlohmann::json::object();
    // Runs the tool. Throw ToolError for a failure the model should see and may retry around; let
    // anything else escape to become an internal error.
    std::function<nlohmann::json(const nlohmann::json &args)> run;
};

class Registry
{
public:
    // A tool whose name is already registered replaces the earlier one, so a caller cannot end up
    // with two entries the lookup picks between arbitrarily.
    void add(Tool tool);

    const Tool *find(const std::string &name) const;

    // The tools/list payload: {"tools": [...]}.
    nlohmann::json list() const;

    std::size_t size() const { return m_tools.size(); }

private:
    std::vector<Tool> m_tools;
};

struct ServerInfo
{
    std::string name    = "orcaslicer";
    std::string version = "0.1.0";
    // The mental model the config schema cannot convey - the object hierarchy, how config layers,
    // that painting is per-facet. Sent once, in the initialize result.
    std::string instructions;
    // Echoed back to the client. MCP asks the server to reply with the version it supports.
    std::string protocol_version = "2025-06-18";
};

// Handles one incoming JSON-RPC message and returns the response to write back.
//
// Returns an **empty string** when there is nothing to send: that is how JSON-RPC notifications
// work, and answering one is a protocol violation rather than a harmless extra. `notifications/
// initialized`, which every client sends right after the handshake, is exactly this case.
std::string handle_message(const std::string &text, const Registry &registry, const ServerInfo &info);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpProtocol_hpp_
