#ifndef slic3r_McpService_hpp_
#define slic3r_McpService_hpp_

#include <memory>

namespace Slic3r { namespace GUI { namespace mcp {

// Owns the MCP server for the application: the dispatcher that gets work onto the GUI thread, the
// registry of tools, and the loopback listener serving them.
//
// Started only when ORCA_MCP is set in the environment. A slicer that silently opens a control
// socket is not something to ship switched on, and an environment variable keeps the spike honest
// about that without yet committing to where the preference should live.
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

private:
    McpService();
    ~McpService();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Convenience for the one call site in GUI_App::post_init().
void start_if_requested();

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpService_hpp_
