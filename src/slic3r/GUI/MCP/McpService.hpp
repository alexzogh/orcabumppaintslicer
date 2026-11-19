#ifndef slic3r_McpService_hpp_
#define slic3r_McpService_hpp_

#include <memory>
#include <string>

namespace Slic3r { namespace GUI { namespace mcp {

// Owns the MCP server for the application: the dispatcher that gets work onto the GUI thread, the
// registry of tools, and the listeners serving them.
//
// Started only when ORCA_MCP is set in the environment. A slicer that silently opens a control
// socket is not something to ship switched on, and an environment variable keeps the spike honest
// about that without yet committing to where the preference should live.
//
// ORCA_MCP_LAN additionally serves the network over TLS, for a client on another device. That is a
// separate variable rather than a value of the first one because it is a materially bigger step:
// the loopback listener is reachable only by processes already running as this user, and the
// network one by anything that can route to this machine.
class McpService
{
public:
    static McpService &instance();

    // No-op unless ORCA_MCP is set. Call from the GUI thread once the main frame exists: the tools
    // reach into Plater, so there has to be one.
    void start();
    void stop();

    bool           is_running() const;
    unsigned short port() const;

    // True when the TLS listener is up as well as the loopback one.
    bool serves_lan() const;

    // Everything a client on another device needs in order to pair.
    //
    // `uri` is the whole of it, and is what the QR code carries. The pieces are kept alongside it
    // rather than left to be parsed back out, because the pairing dialog shows the address and the
    // fingerprint as text for a person to check against what their device reports, and a dialog
    // picking a URI apart to do that would be a second, quieter copy of the format.
    struct Pairing
    {
        std::string    host;
        unsigned short port = 0;
        std::string    fingerprint;
        std::string    uri;

        // False when the network listener is not running, or when the machine has no non-loopback
        // address to advertise.
        bool valid() const { return !uri.empty(); }
    };

    Pairing pairing() const;

private:
    McpService();
    ~McpService();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// What the pairing QR code carries: everything the client needs and nothing it could look up for
// itself - where to connect, the token to present, and the one certificate to accept.
//
// Declared here because the exact shape of this string is a contract with every client that has
// ever been paired, so it is worth pinning down in a test rather than leaving to a format string.
std::string make_pairing_uri(const std::string &host,
                             unsigned short     port,
                             const std::string &token,
                             const std::string &fingerprint);

// Convenience for the one call site in GUI_App::post_init().
void start_if_requested();

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpService_hpp_
