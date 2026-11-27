#include "McpListener.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Thread.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace {

// The one endpoint MCP is served on, so a stray probe of / never reaches the protocol layer.
constexpr const char *mcp_path = "/mcp";

// Tool arguments are small; the largest thing expected is a base64 PNG for a texture layer.
constexpr std::uint64_t max_body_bytes = 16ull * 1024 * 1024;

constexpr std::chrono::seconds idle_timeout{30};

std::string make_token()
{
    std::random_device                 rd;
    std::uniform_int_distribution<int> nibble(0, 15);
    static const char                  digits[] = "0123456789abcdef";

    std::string out;
    out.reserve(64);
    for (int i = 0; i < 64; ++i)
        out.push_back(digits[nibble(rd)]);
    return out;
}

// Compared without an early exit, so how long the comparison takes does not tell a local process
// how much of a guessed token was right.
bool token_equals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

// A page in a browser on this machine can reach a loopback port, so one the user happens to have
// open could try to drive the slicer. It cannot attach an Authorization header cross-origin without
// a successful preflight, and no preflight is ever answered here - but refusing a foreign Origin
// outright means that never has to be reasoned about again.
bool origin_allowed(const std::string &origin)
{
    if (origin.empty() || origin == "null")
        return true;
    return origin.rfind("http://127.0.0.1", 0) == 0 || origin.rfind("http://localhost", 0) == 0 ||
           origin.rfind("https://127.0.0.1", 0) == 0 || origin.rfind("https://localhost", 0) == 0;
}

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket, const Registry &registry, const ServerInfo &info, std::string token)
        : m_stream(std::move(socket)), m_registry(registry), m_info(info), m_token(std::move(token))
    {}

    void run() { do_read(); }

private:
    void do_read()
    {
        m_parser.emplace();
        m_parser->body_limit(max_body_bytes);
        m_stream.expires_after(idle_timeout);
        http::async_read(m_stream, m_buffer, *m_parser, beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t)
    {
        if (ec) {
            // end_of_stream is the ordinary way a keep-alive connection ends, not a fault.
            if (ec != http::error::end_of_stream && ec != beast::error::timeout)
                BOOST_LOG_TRIVIAL(debug) << "MCP session read failed: " << ec.message();
            close();
            return;
        }
        send(build_response(m_parser->get()));
    }

    http::response<http::string_body> build_response(const http::request<http::string_body> &req) const
    {
        const auto make = [&req](http::status status, std::string body, const char *content_type) {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::server, "OrcaSlicer-MCP");
            res.set(http::field::content_type, content_type);
            res.keep_alive(req.keep_alive());
            res.body() = std::move(body);
            res.prepare_payload();
            return res;
        };

        if (!origin_allowed(std::string(req[http::field::origin])))
            return make(http::status::forbidden, "forbidden origin", "text/plain");
        if (req.target() != mcp_path)
            return make(http::status::not_found, "not found", "text/plain");
        if (req.method() != http::verb::post)
            return make(http::status::method_not_allowed, "use POST", "text/plain");

        static constexpr char bearer[] = "Bearer ";
        const std::string     auth(req[http::field::authorization]);
        if (auth.rfind(bearer, 0) != 0 || !token_equals(auth.substr(std::strlen(bearer)), m_token))
            return make(http::status::unauthorized, "unauthorized", "text/plain");

        const std::string out = handle_message(req.body(), m_registry, m_info);
        // A notification produces nothing to send. 202 with an empty body is what the MCP HTTP
        // transport asks for; a 200 with an empty body would read as a truncated response.
        return out.empty() ? make(http::status::accepted, {}, "application/json")
                           : make(http::status::ok, out, "application/json");
    }

    void send(http::response<http::string_body> &&res)
    {
        auto held = std::make_shared<http::response<http::string_body>>(std::move(res));
        m_response = held;
        const bool keep_alive = held->keep_alive();
        http::async_write(m_stream, *held,
                          [self = shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
                              if (ec) {
                                  self->close();
                                  return;
                              }
                              if (keep_alive)
                                  self->do_read();
                              else
                                  self->close();
                          });
    }

    void close()
    {
        beast::error_code ignored;
        m_stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    }

    beast::tcp_stream                                           m_stream;
    beast::flat_buffer                                          m_buffer;
    std::optional<http::request_parser<http::string_body>>      m_parser;
    std::shared_ptr<http::response<http::string_body>>          m_response;
    const Registry                                             &m_registry;
    const ServerInfo                                           &m_info;
    std::string                                                 m_token;
};

} // namespace

struct McpListener::Impl
{
    Registry    registry;
    ServerInfo  info;
    std::string token = make_token();

    net::io_context   ioc{1};
    tcp::acceptor     acceptor{ioc};
    std::thread       thread;
    std::atomic<bool> running{false};
    unsigned short    bound_port = 0;

    Impl(Registry r, ServerInfo i) : registry(std::move(r)), info(std::move(i)) {}

    void do_accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                // operation_aborted is stop() closing the acceptor, which is not a failure.
                if (ec != net::error::operation_aborted)
                    BOOST_LOG_TRIVIAL(error) << "MCP accept failed: " << ec.message();
                return;
            }
            std::make_shared<Session>(std::move(socket), registry, info, token)->run();
            do_accept();
        });
    }
};

McpListener::McpListener(Registry registry, ServerInfo info)
    : m_impl(std::make_unique<Impl>(std::move(registry), std::move(info)))
{}

McpListener::~McpListener() { stop(); }

unsigned short McpListener::start()
{
    if (m_impl->running)
        return m_impl->bound_port;

    beast::error_code ec;
    // Loopback, and an ephemeral port: a fixed one is a port a hostile local process can squat on
    // before the slicer starts, and the client has to read the port out of the handshake file
    // either way. SO_REUSEADDR is deliberately left unset - on Windows it would let another process
    // bind the same port out from under this one.
    const tcp::endpoint endpoint{net::ip::make_address("127.0.0.1"), 0};

    m_impl->acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener could not open a socket: " << ec.message();
        return 0;
    }
    m_impl->acceptor.bind(endpoint, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener could not bind loopback: " << ec.message();
        return 0;
    }
    m_impl->acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener could not listen: " << ec.message();
        return 0;
    }

    const tcp::endpoint bound = m_impl->acceptor.local_endpoint(ec);
    if (ec)
        return 0;
    m_impl->bound_port = bound.port();

    m_impl->running = true;
    m_impl->do_accept();

    Impl *impl    = m_impl.get();
    m_impl->thread = std::thread([impl] {
        set_current_thread_name("mcp_listener");
        impl->ioc.run();
    });

    BOOST_LOG_TRIVIAL(info) << "MCP listener on 127.0.0.1:" << m_impl->bound_port;
    return m_impl->bound_port;
}

void McpListener::stop()
{
    if (!m_impl || !m_impl->running)
        return;
    m_impl->running = false;

    beast::error_code ignored;
    m_impl->acceptor.close(ignored);
    m_impl->ioc.stop();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
}

bool McpListener::is_running() const { return m_impl && m_impl->running; }

unsigned short McpListener::port() const { return m_impl ? m_impl->bound_port : 0; }

const std::string &McpListener::token() const { return m_impl->token; }

}}} // namespace Slic3r::GUI::mcp
