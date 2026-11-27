#ifndef slic3r_McpListener_hpp_
#define slic3r_McpListener_hpp_

#include <memory>
#include <string>

#include "McpProtocol.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

// Serves the MCP protocol layer over HTTP on the loopback interface.
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
    McpListener(Registry registry, ServerInfo info);
    ~McpListener();

    McpListener(const McpListener &)            = delete;
    McpListener &operator=(const McpListener &) = delete;

    // Binds 127.0.0.1 on an ephemeral port and starts serving on a thread of its own. Returns the
    // bound port, or 0 if it could not start.
    //
    // Ephemeral deliberately: a fixed port is one a hostile local process can squat on before the
    // slicer starts, and the client has to read the port from the handshake file regardless.
    unsigned short start();
    void           stop();

    bool           is_running() const;
    unsigned short port() const;

    // The bearer token every request must carry, generated when the listener is constructed. The
    // caller is responsible for writing it, with the port, somewhere only this user can read -
    // which is the GUI's business, not this class's.
    const std::string &token() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpListener_hpp_
