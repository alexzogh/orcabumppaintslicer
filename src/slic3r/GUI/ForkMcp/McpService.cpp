#include "McpService.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/thread.h>

#include "MainThreadDispatcher.hpp"
#include "McpListener.hpp"
#include "McpProtocol.hpp"
#include "McpTools.hpp"

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

Registry build_registry(MainThreadDispatcher &dispatcher)
{
    // Each tool group registers itself. The dispatcher is reached through a plain callable so a
    // group needs to know nothing about wxWidgets, or about which thread it is being called on.
    ToolContext context;
    context.on_gui = [&dispatcher](const std::function<void()> &fn) {
        const MainThreadDispatcher::Status status = dispatcher.call(fn, tool_timeout);
        if (status != MainThreadDispatcher::Status::Ok)
            throw ToolError(describe(status));
    };

    Registry registry;
    add_project_tools(registry, context);
    add_paint_tools(registry, context);
    add_config_tools(registry, context);
    add_texture_tools(registry, context);
    add_output_tools(registry, context);
    add_app_tools(registry, context);
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

// Where a client finds the port and token. data_dir() is per-user, which is the bulk of the
// protection; write_handshake() narrows the file itself to the owner on top of that.
boost::filesystem::path handshake_path() { return boost::filesystem::path(data_dir()) / "mcp.json"; }

// Write the handshake file so that only this user can read it.
//
// The file carries the bearer token, so it is created empty and narrowed before anything is written
// into it: setting the permissions afterwards would leave a window in which the token sits in a
// world-readable file. An existing file is removed rather than truncated, since truncating keeps
// whatever permissions it already had.
//
// owner_read | owner_write is POSIX. On Windows boost maps this onto the read-only attribute rather
// than a real ACL, so there the per-user data_dir stays the whole of the protection.
bool write_handshake(const boost::filesystem::path &path, const std::string &contents)
{
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);

    {
        std::ofstream create(path.string(), std::ios::binary | std::ios::trunc);
        if (!create)
            return false;
    }

    boost::filesystem::permissions(path, boost::filesystem::owner_read | boost::filesystem::owner_write, ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "MCP service could not narrow " << path.string() << ": " << ec.message();

    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << contents;
    out.flush();
    return bool(out);
}

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
        const std::string contents =
            json{{"port", port}, {"token", listener->token()}, {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}}
                .dump(2);
        if (!write_handshake(path, contents))
            throw std::runtime_error("could not write the handshake file");
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
