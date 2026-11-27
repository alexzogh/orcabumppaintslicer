#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/ForkMcp/McpProtocol.hpp"

using Slic3r::GUI::mcp::Registry;
using Slic3r::GUI::mcp::ServerInfo;
using Slic3r::GUI::mcp::Tool;
using Slic3r::GUI::mcp::ToolError;
using Slic3r::GUI::mcp::handle_message;
using nlohmann::json;

namespace {

// Four tools covering the outcomes the protocol layer has to tell apart: an ordinary success, a
// tool that fails the way a real one fails, a tool that breaks, and one that builds its own
// content blocks instead of returning a value to be wrapped.
Registry make_registry(bool *echo_ran = nullptr)
{
    Registry registry;

    Tool echo;
    echo.name         = "echo";
    echo.description  = "Returns its message argument";
    echo.input_schema = json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}};
    echo.run          = [echo_ran](const json &args) -> json {
        if (echo_ran != nullptr)
            *echo_ran = true;
        return args.value("message", std::string("(nothing)"));
    };
    registry.add(std::move(echo));

    Tool failing;
    failing.name        = "no_such_object";
    failing.description = "Always fails the way a real tool fails";
    failing.run         = [](const json &) -> json { throw ToolError("No object with id 7"); };
    registry.add(std::move(failing));

    Tool broken;
    broken.name        = "broken";
    broken.description = "Throws something that is not a ToolError";
    broken.run         = [](const json &) -> json { throw std::runtime_error("vector subscript out of range"); };
    registry.add(std::move(broken));

    Tool blocks;
    blocks.name        = "own_blocks";
    blocks.description = "Builds its own content blocks";
    blocks.run         = [](const json &) -> json {
        return json{{"content", json::array({json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}})}};
    };
    registry.add(std::move(blocks));

    return registry;
}

ServerInfo make_info()
{
    ServerInfo info;
    info.name         = "orcaslicer";
    info.version      = "0.1.0";
    info.instructions = "Plates contain objects contain volumes.";
    return info;
}

// Parses what the server wrote back, or null when it wrote nothing at all.
json reply(const std::string &request, const Registry &registry, const ServerInfo &info)
{
    const std::string out = handle_message(request, registry, info);
    return out.empty() ? json(nullptr) : json::parse(out);
}

// tools/list order is not part of the contract, so tests look entries up by name.
json listed_tool(const json &tools, const std::string &name)
{
    const auto it = std::find_if(tools.begin(), tools.end(),
                                 [&name](const json &tool) { return tool.value("name", std::string()) == name; });
    return it == tools.end() ? json(nullptr) : *it;
}

} // namespace

TEST_CASE("text that is not JSON is a parse error", "[McpProtocol]")
{
    const json r = reply("{not json", make_registry(), make_info());

    CHECK(r["error"]["code"] == -32700);
    // There is no id to answer against when the request could not be read at all.
    CHECK(r["id"].is_null());
    CHECK(r["jsonrpc"] == "2.0");
}

TEST_CASE("a message that is not a JSON-RPC 2.0 request is rejected", "[McpProtocol]")
{
    const auto request = GENERATE(as<std::string>{},
                                  R"([1,2,3])",
                                  R"({"id":1,"method":"ping"})",
                                  R"({"jsonrpc":"1.0","id":1,"method":"ping"})",
                                  R"({"jsonrpc":"2.0","id":1})");
    CAPTURE(request);

    CHECK(reply(request, make_registry(), make_info())["error"]["code"] == -32600);
}

TEST_CASE("an unknown method is method not found", "[McpProtocol]")
{
    CHECK(reply(R"({"jsonrpc":"2.0","id":1,"method":"nope"})", make_registry(), make_info())["error"]["code"] ==
          -32601);
}

TEST_CASE("a notification is never answered", "[McpProtocol]")
{
    // Answering one is a protocol violation rather than a harmless extra, including when the
    // method is unknown and there is something the server would like to say about it.
    const auto request = GENERATE(as<std::string>{},
                                  R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                                  R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
                                  R"({"jsonrpc":"2.0","method":"nope"})");
    CAPTURE(request);

    CHECK(handle_message(request, make_registry(), make_info()).empty());
}

TEST_CASE("initialize reports what the server is", "[McpProtocol]")
{
    const ServerInfo info = make_info();
    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})", make_registry(), info)["result"];

    CHECK(r["protocolVersion"] == info.protocol_version);
    CHECK(r["capabilities"].contains("tools"));
    CHECK(r["serverInfo"]["name"] == "orcaslicer");
    CHECK(r["serverInfo"]["version"] == "0.1.0");
    CHECK(r["instructions"] == info.instructions);
}

TEST_CASE("initialize omits the instructions when there are none", "[McpProtocol]")
{
    ServerInfo info = make_info();
    info.instructions.clear();

    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})", make_registry(), info)["result"];

    CHECK_FALSE(r.contains("instructions"));
}

TEST_CASE("ping answers with an empty result", "[McpProtocol]")
{
    CHECK(reply(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", make_registry(), make_info())["result"].empty());
}

TEST_CASE("a request id comes back unchanged", "[McpProtocol]")
{
    const Registry   registry = make_registry();
    const ServerInfo info     = make_info();

    CHECK(reply(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})", registry, info)["id"] == "abc");
    CHECK(reply(R"({"jsonrpc":"2.0","id":7,"method":"ping"})", registry, info)["id"] == 7);
}

TEST_CASE("tools/list reports every registered tool", "[McpProtocol]")
{
    const json tools =
        reply(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", make_registry(), make_info())["result"]["tools"];

    REQUIRE(tools.is_array());
    CHECK(tools.size() == 4);

    const json echo = listed_tool(tools, "echo");
    REQUIRE(echo.is_object());
    CHECK(echo["description"] == "Returns its message argument");
    CHECK(echo["inputSchema"]["type"] == "object");
}

TEST_CASE("a tool result comes back as a content block", "[McpProtocol]")
{
    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"tools/call",
                             "params":{"name":"echo","arguments":{"message":"hello"}}})",
                         make_registry(), make_info())["result"];

    CHECK(r["content"][0]["type"] == "text");
    // A string result is the text, not a JSON-encoded string that the model would have to unwrap.
    CHECK(r["content"][0]["text"] == "hello");
    CHECK_FALSE(r.contains("isError"));
}

TEST_CASE("a tool that builds its own content blocks passes them through", "[McpProtocol]")
{
    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"own_blocks"}})",
                         make_registry(), make_info())["result"];

    CHECK(r["content"][0]["type"] == "image");
    CHECK(r["content"][0]["mimeType"] == "image/png");
}

TEST_CASE("a tool failure is a successful call flagged as an error", "[McpProtocol]")
{
    // The distinction the whole layer turns on: a tool that fails is a successful JSON-RPC call
    // whose result says it failed, so the model reads it as a turn and can try something else.
    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"no_such_object"}})",
                         make_registry(), make_info());

    CHECK_FALSE(r.contains("error"));
    CHECK(r["result"]["isError"] == true);
    CHECK(r["result"]["content"][0]["text"] == "No object with id 7");
}

TEST_CASE("an unexpected exception inside a tool is an internal error", "[McpProtocol]")
{
    // A tool that broke is the server's problem, and not something the model can route around.
    const json r = reply(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"broken"}})",
                         make_registry(), make_info());

    CHECK(r["error"]["code"] == -32603);
}

TEST_CASE("a tools/call that does not name a registered tool is invalid params", "[McpProtocol]")
{
    const auto request = GENERATE(as<std::string>{},
                                  R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"ghost"}})",
                                  R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{}})",
                                  R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})");
    CAPTURE(request);

    CHECK(reply(request, make_registry(), make_info())["error"]["code"] == -32602);
}

TEST_CASE("a tool call sent as a notification does not run", "[McpProtocol]")
{
    // Deliberately stricter than JSON-RPC requires. A fire-and-forget call cannot report what it
    // did, and these tools change the open project, so it is dropped rather than run silently.
    bool           ran      = false;
    const Registry registry = make_registry(&ran);

    const std::string out = handle_message(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"echo","arguments":{"message":"x"}}})", registry,
        make_info());

    CHECK(out.empty());
    CHECK_FALSE(ran);
}

TEST_CASE("registering a name twice replaces the earlier tool", "[McpProtocol]")
{
    Registry registry;

    Tool first;
    first.name        = "dup";
    first.description = "first";
    first.run         = [](const json &) -> json { return "first"; };
    registry.add(std::move(first));

    Tool second;
    second.name        = "dup";
    second.description = "second";
    second.run         = [](const json &) -> json { return "second"; };
    registry.add(std::move(second));

    CHECK(registry.size() == 1);
    REQUIRE(registry.find("dup") != nullptr);
    CHECK(registry.find("dup")->description == "second");
    CHECK(registry.find("missing") == nullptr);
}
