#include "McpService.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/thread.h>

#include "MainThreadDispatcher.hpp"
#include "McpIdentity.hpp"
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
    add_geometry_tools(registry, context);
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
// protection; write_private_file() narrows the file itself to the owner on top of that.
boost::filesystem::path handshake_path() { return boost::filesystem::path(data_dir()) / "mcp.json"; }

bool env_flag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

// The address to tell a phone to connect to.
//
// A machine can have several - a VPN, a virtualisation bridge, a second adapter - and nothing here
// can tell which one the phone shares a network with, so this takes the first non-loopback IPv4
// address the resolver reports for the host name and leaves correcting it to whoever is looking at
// the pairing code. Empty means no usable address was found, which is what being off the network
// looks like; the listener still runs, there is just nothing useful to show.
std::string local_address()
{
    namespace net = boost::asio;
    using tcp     = net::ip::tcp;
    try {
        net::io_context           ioc;
        tcp::resolver             resolver(ioc);
        boost::system::error_code ec;
        const auto                results = resolver.resolve(net::ip::host_name(), "", ec);
        if (ec)
            return {};
        for (const auto &entry : results) {
            const auto address = entry.endpoint().address();
            if (address.is_v4() && !address.is_loopback() && !address.is_unspecified())
                return address.to_string();
        }
    } catch (const std::exception &) {
        // Resolving the host name is allowed to fail - a machine with no configured name, or no
        // network at all. That is not an error worth reporting, it just means no pairing address.
    }
    return {};
}

} // namespace

// What the pairing QR code carries: everything the client needs and nothing it could look up for
// itself - where to connect, the token to present, and the one certificate to accept. No
// percent-encoding, because every field is either an IPv4 literal or lowercase hex.
std::string make_pairing_uri(const std::string &host,
                             unsigned short     port,
                             const std::string &token,
                             const std::string &fingerprint)
{
    return "orcaslicer://pair?host=" + host + "&port=" + std::to_string(port) + "&token=" + token +
           "&fp=" + fingerprint;
}

struct McpService::Impl
{
    // Declared before the listener so it outlives it: the tools hold a reference to this, and the
    // listener owns the tools.
    MainThreadDispatcher dispatcher{[](std::function<void()> fn) { wxGetApp().CallAfter(std::move(fn)); },
                                    [] { return wxThread::IsMain(); }};
    // Two listeners rather than one that serves both, because the two have different terms of
    // entry: the loopback one speaks plaintext to clients that read its token off this disk, and
    // the network one speaks TLS to clients that were handed its token by a person. They hold
    // separate tokens, so unpairing a phone cannot lock out a local client and vice versa.
    std::unique_ptr<McpListener> listener;
    std::unique_ptr<McpListener> lan_listener;
    McpService::Pairing          pairing;
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

    json handshake{{"port", port},
                   {"token", listener->token()},
                   {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}};

    // Serving the network is a second, louder opt-in than serving this machine. ORCA_MCP alone
    // reaches only processes already running as this user; this one reaches everything on the
    // network, so it is not something to get by setting the first variable slightly differently.
    std::unique_ptr<McpListener> lan;
    if (env_flag("ORCA_MCP_LAN")) {
        const Identity identity = load_or_create_identity(boost::filesystem::path(data_dir()));
        if (!identity.valid()) {
            BOOST_LOG_TRIVIAL(error) << "MCP service has no TLS identity, so it is not serving the network";
        } else {
            ListenerOptions options;
            options.lan             = true;
            options.certificate_pem = identity.certificate_pem;
            options.private_key_pem = identity.private_key_pem;

            auto candidate = std::make_unique<McpListener>(build_registry(m_impl->dispatcher),
                                                           build_server_info(), std::move(options));
            if (const unsigned short lan_port = candidate->start(); lan_port == 0) {
                BOOST_LOG_TRIVIAL(error) << "MCP service could not start a listener on the network";
            } else {
                const std::string host = local_address();
                if (!host.empty()) {
                    m_impl->pairing.host        = host;
                    m_impl->pairing.port        = lan_port;
                    m_impl->pairing.fingerprint = identity.fingerprint_sha256;
                    m_impl->pairing.uri = make_pairing_uri(host, lan_port, candidate->token(),
                                                           identity.fingerprint_sha256);
                }
                handshake["lan"] = json{{"port", lan_port},
                                        {"token", candidate->token()},
                                        {"host", host},
                                        {"url", host.empty() ? std::string()
                                                             : "https://" + host + ":" + std::to_string(lan_port) + "/mcp"},
                                        {"fingerprint_sha256", identity.fingerprint_sha256},
                                        {"pairing_uri", m_impl->pairing.uri}};
                lan = std::move(candidate);
            }
        }
    }

    const boost::filesystem::path path = handshake_path();
    try {
        if (!write_private_file(path, handshake.dump(2)))
            throw std::runtime_error("could not write the handshake file");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP service could not write " << path.string() << ": " << e.what();
        listener->stop();
        if (lan)
            lan->stop();
        return;
    }

    m_impl->listener     = std::move(listener);
    m_impl->lan_listener = std::move(lan);
    BOOST_LOG_TRIVIAL(info) << "MCP service listening on 127.0.0.1:" << port << ", handshake at " << path.string();
    if (m_impl->lan_listener)
        BOOST_LOG_TRIVIAL(info) << "MCP service also listening on the network, port " << m_impl->lan_listener->port()
                                << " (TLS)";
}

void McpService::stop()
{
    if (!m_impl || (!m_impl->listener && !m_impl->lan_listener))
        return;
    // The network one first: it is the one with something listening on it from outside.
    if (m_impl->lan_listener) {
        m_impl->lan_listener->stop();
        m_impl->lan_listener.reset();
    }
    if (m_impl->listener) {
        m_impl->listener->stop();
        m_impl->listener.reset();
    }
    m_impl->pairing = Pairing{};
    m_impl->dispatcher.shutdown();

    boost::system::error_code ignored;
    boost::filesystem::remove(handshake_path(), ignored);
}

bool McpService::serves_lan() const { return m_impl && m_impl->lan_listener && m_impl->lan_listener->is_running(); }

McpService::Pairing McpService::pairing() const { return m_impl ? m_impl->pairing : Pairing{}; }

bool McpService::is_running() const { return m_impl && m_impl->listener && m_impl->listener->is_running(); }

unsigned short McpService::port() const { return (m_impl && m_impl->listener) ? m_impl->listener->port() : 0; }

bool requested() { return env_flag("ORCA_MCP"); }

void start_if_requested()
{
    if (!requested())
        return;
    McpService::instance().start();
}

}}} // namespace Slic3r::GUI::mcp
