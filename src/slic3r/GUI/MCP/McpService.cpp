#include "McpService.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/thread.h>

#include "MainThreadDispatcher.hpp"
#include "McpListener.hpp"
#include "McpProtocol.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// Long enough that an ordinary main-thread operation is never cut off, short enough that a client
// blocked behind a modal dialog finds out rather than waiting forever.
constexpr std::chrono::milliseconds tool_timeout{5000};

std::string describe(MainThreadDispatcher::Status status)
{
    switch (status) {
    case MainThreadDispatcher::Status::TimedOutNotRun:
        return "OrcaSlicer's interface is busy and did not start this within " +
               std::to_string(tool_timeout.count()) + " ms. Nothing was changed; it is safe to retry.";
    case MainThreadDispatcher::Status::TimedOutMaybeRan:
        return "OrcaSlicer's interface did not finish this within " + std::to_string(tool_timeout.count()) +
               " ms. It had already begun and cannot be cancelled, so whether it completed is unknown - "
               "check the current state before retrying.";
    case MainThreadDispatcher::Status::ShuttingDown:
        return "OrcaSlicer is shutting down.";
    case MainThreadDispatcher::Status::Ok:
        break;
    }
    return {};
}

// Runs `fn` on the GUI thread and turns anything other than success into a ToolError, so each tool
// body can read as if it were simply running there.
void on_gui_thread(MainThreadDispatcher &dispatcher, const std::function<void()> &fn)
{
    const MainThreadDispatcher::Status status = dispatcher.call(fn, tool_timeout);
    if (status != MainThreadDispatcher::Status::Ok)
        throw ToolError(describe(status));
}

Registry build_registry(MainThreadDispatcher &dispatcher)
{
    Registry registry;

    Tool list_objects;
    list_objects.name        = "list_objects";
    list_objects.description = "Lists the objects on the current plate, with their ids, names, instance counts "
                               "and the volumes (parts) each is made of. Object ids are stable; use them rather "
                               "than positions, which shift when an object is removed.";
    list_objects.input_schema = json{{"type", "object"}, {"properties", json::object()}};
    list_objects.run          = [&dispatcher](const json &) -> json {
        json        objects = json::array();
        std::string failure;

        on_gui_thread(dispatcher, [&objects, &failure] {
            Plater *plater = wxGetApp().plater();
            if (plater == nullptr) {
                failure = "No plater is open.";
                return;
            }
            for (const ModelObject *object : plater->model().objects) {
                if (object == nullptr)
                    continue;
                json volumes = json::array();
                for (const ModelVolume *volume : object->volumes)
                    if (volume != nullptr)
                        volumes.push_back(json{{"id", volume->id().id}, {"name", volume->name}});
                objects.push_back(json{{"id", object->id().id},
                                       {"name", object->name},
                                       {"instances", object->instances.size()},
                                       {"volumes", std::move(volumes)}});
            }
        });

        if (!failure.empty())
            throw ToolError(failure);
        return json{{"objects", std::move(objects)}};
    };
    registry.add(std::move(list_objects));

    return registry;
}

ServerInfo build_server_info()
{
    ServerInfo info;
    info.name    = "orcaslicer";
    info.version = "0.1.0";
    // The mental model the config schema cannot convey. Everything else is discoverable.
    info.instructions =
        "OrcaSlicer, a 3D printing slicer.\n"
        "- A plate holds objects; an object holds volumes (parts); painting is per-facet and lives on "
        "the volume, not the object.\n"
        "- Settings layer: printer < print < filament < per-object override.\n"
        "- Lengths are millimetres.\n"
        "- Address objects by id, never by position in a list.\n";
    return info;
}

// Where a client finds the port and token. data_dir() is already per-user, which is the bulk of the
// protection; tightening the file's own ACL is still owed before this is anything but a spike.
boost::filesystem::path handshake_path() { return boost::filesystem::path(data_dir()) / "mcp.json"; }

} // namespace

struct McpService::Impl
{
    // Declared before the listener so it outlives it: the tools hold a reference to this, and the
    // listener owns the tools.
    MainThreadDispatcher dispatcher{[](std::function<void()> fn) { wxGetApp().CallAfter(std::move(fn)); },
                                    [] { return wxThread::IsMain(); }};
    std::unique_ptr<McpListener> listener;
};

McpService::McpService() : m_impl(std::make_unique<Impl>()) {}
McpService::~McpService() { stop(); }

McpService &McpService::instance()
{
    static McpService service;
    return service;
}

void McpService::start()
{
    if (m_impl->listener)
        return;

    auto listener = std::make_unique<McpListener>(build_registry(m_impl->dispatcher), build_server_info());
    const unsigned short port = listener->start();
    if (port == 0) {
        BOOST_LOG_TRIVIAL(error) << "MCP service could not start a listener";
        return;
    }

    const boost::filesystem::path path = handshake_path();
    try {
        std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        out << json{{"port", port}, {"token", listener->token()}, {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}}
                   .dump(2);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP service could not write " << path.string() << ": " << e.what();
        listener->stop();
        return;
    }

    m_impl->listener = std::move(listener);
    BOOST_LOG_TRIVIAL(info) << "MCP service listening on 127.0.0.1:" << port << ", handshake at " << path.string();
}

void McpService::stop()
{
    if (!m_impl || !m_impl->listener)
        return;
    m_impl->listener->stop();
    m_impl->listener.reset();
    m_impl->dispatcher.shutdown();

    boost::system::error_code ignored;
    boost::filesystem::remove(handshake_path(), ignored);
}

bool McpService::is_running() const { return m_impl && m_impl->listener && m_impl->listener->is_running(); }

unsigned short McpService::port() const { return (m_impl && m_impl->listener) ? m_impl->listener->port() : 0; }

void start_if_requested()
{
    const char *enabled = std::getenv("ORCA_MCP");
    if (enabled == nullptr || *enabled == '\0' || std::string(enabled) == "0")
        return;
    McpService::instance().start();
}

}}} // namespace Slic3r::GUI::mcp
