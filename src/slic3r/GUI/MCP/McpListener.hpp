#ifndef slic3r_McpListener_hpp_
#define slic3r_McpListener_hpp_

#include <memory>
#include <string>

#include "McpProtocol.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

// How a listener is exposed.
//
// The two shapes are deliberately different, because the threats are. A loopback listener is
// reachable only by processes already running as this user, so a bearer token read from a file
// only this user can open is the whole of the protection. A listener on the network is reachable
// by anything on it, so the token must not cross the wire readable, which means TLS.
//
// The listener refuses to start on the LAN without an identity rather than quietly falling back to
// plaintext: a control channel that can delete objects, rewrite settings and export G-code is not
// something to serve in the clear because a certificate failed to load.
struct ListenerOptions
{
    // False binds 127.0.0.1; true binds every IPv4 interface. IPv6 is not bound - a dual-stack
    // socket is a separate decision, and nothing that pairs over a QR code on a home network needs
    // it yet.
    bool lan = false;

    // PEM, as McpIdentity produces them. Both must be set for a TLS listener and both must be
    // empty for a plaintext one.
    std::string certificate_pem;
    std::string private_key_pem;

    bool serves_tls() const { return !certificate_pem.empty() && !private_key_pem.empty(); }
};

// Whether a request carrying this Origin header may be served by a listener in this mode.
//
// Declared here rather than kept private because it is the rule that stops a web page the user
// happens to have open from driving the slicer, and a rule like that is worth asserting on
// directly rather than inferring from the behaviour of a socket.
bool origin_allowed(const std::string &origin, bool lan);

// Serves the MCP protocol layer over HTTP, or HTTPS when an identity is supplied.
//
// A sibling of GUI/HttpServer rather than an extension of it. That server parses HTTP by hand, a
// line at a time, and its read_body() reads a fixed 1000 bytes and then closes the connection
// without answering - fine for the OAuth callback it was written for, which is a bare GET, and a
// poor foundation for request bodies, keep-alive and chunked encoding. Beast is already a
// dependency and already parses all of that correctly, so this uses it directly and leaves the
// OAuth path - security-adjacent code on a published redirect URI - untouched.
//
// Tools are expected to have captured a MainThreadDispatcher themselves: this class knows nothing
// about the slicer, and nothing about which thread anything has to run on.
class McpListener
{
public:
    McpListener(Registry registry, ServerInfo info, ListenerOptions options = {});
    ~McpListener();

    McpListener(const McpListener &)            = delete;
    McpListener &operator=(const McpListener &) = delete;

    // Binds the configured interface on an ephemeral port and starts serving on a thread of its
    // own. Returns the bound port, or 0 if it could not start.
    //
    // Ephemeral deliberately: a fixed port is one a hostile local process can squat on before the
    // slicer starts, and the client has to read the port from the handshake file regardless.
    unsigned short start();
    void           stop();

    bool           is_running() const;
    unsigned short port() const;
    bool           serves_tls() const;

    // The bearer token every request must carry, generated when the listener is constructed. The
    // caller is responsible for writing it, with the port, somewhere only this user can read -
    // which is the GUI's business, not this class's.
    //
    // Each listener has its own, so the token a phone was paired with is not the token a local
    // client holds: revoking one does not disturb the other.
    const std::string &token() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpListener_hpp_
