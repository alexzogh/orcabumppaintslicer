#include "McpListener.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/log/trivial.hpp>

#include "McpIdentity.hpp"

#include "libslic3r/Thread.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp       = net::ip::tcp;

namespace {

using PlainStream = beast::tcp_stream;
using TlsStream   = beast::ssl_stream<beast::tcp_stream>;

// The one endpoint MCP is served on, so a stray probe of / never reaches the protocol layer.
constexpr const char *mcp_path = "/mcp";

// Tool arguments are small; the largest thing expected is a base64 PNG for a texture layer.
constexpr std::uint64_t max_body_bytes = 16ull * 1024 * 1024;

constexpr std::chrono::seconds idle_timeout{30};

// Compared without an early exit, so how long the comparison takes does not tell a caller how much
// of a guessed token was right.
bool token_equals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}


// Everything about answering a request that does not depend on how the bytes arrived, so the
// plaintext and TLS sessions below cannot drift apart on the part that decides who gets in.
http::response<http::string_body> build_response(const http::request<http::string_body> &req,
                                                 const Registry                         &registry,
                                                 const ServerInfo                       &info,
                                                 const std::string                      &token,
                                                 bool                                    lan)
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

    if (!origin_allowed(std::string(req[http::field::origin]), lan))
        return make(http::status::forbidden, "forbidden origin", "text/plain");
    if (req.target() != mcp_path)
        return make(http::status::not_found, "not found", "text/plain");
    if (req.method() != http::verb::post)
        return make(http::status::method_not_allowed, "use POST", "text/plain");

    static constexpr char bearer[] = "Bearer ";
    const std::string     auth(req[http::field::authorization]);
    if (auth.rfind(bearer, 0) != 0 || !token_equals(auth.substr(std::strlen(bearer)), token))
        return make(http::status::unauthorized, "unauthorized", "text/plain");

    const std::string out = handle_message(req.body(), registry, info);
    // A notification produces nothing to send. 202 with an empty body is what the MCP HTTP
    // transport asks for; a 200 with an empty body would read as a truncated response.
    return out.empty() ? make(http::status::accepted, {}, "application/json")
                       : make(http::status::ok, out, "application/json");
}

template<typename Stream> class Session : public std::enable_shared_from_this<Session<Stream>>
{
    static constexpr bool is_tls = std::is_same_v<Stream, TlsStream>;

public:
    Session(Stream stream, const Registry &registry, const ServerInfo &info, std::string token, bool lan)
        : m_stream(std::move(stream)), m_registry(registry), m_info(info), m_token(std::move(token)), m_lan(lan)
    {}

    void run()
    {
        if constexpr (is_tls) {
            // The handshake is on the clock too: an opened connection that never gets as far as
            // offering a ClientHello would otherwise hold a slot indefinitely.
            beast::get_lowest_layer(m_stream).expires_after(idle_timeout);
            m_stream.async_handshake(ssl::stream_base::server,
                                     [self = this->shared_from_this()](beast::error_code ec) {
                                         if (ec) {
                                             BOOST_LOG_TRIVIAL(debug) << "MCP TLS handshake failed: " << ec.message();
                                             return;
                                         }
                                         self->do_read();
                                     });
        } else {
            do_read();
        }
    }

private:
    void do_read()
    {
        m_parser.emplace();
        m_parser->body_limit(max_body_bytes);
        beast::get_lowest_layer(m_stream).expires_after(idle_timeout);
        http::async_read(m_stream, m_buffer, *m_parser,
                         beast::bind_front_handler(&Session::on_read, this->shared_from_this()));
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
        send(build_response(m_parser->get(), m_registry, m_info, m_token, m_lan));
    }

    void send(http::response<http::string_body> &&res)
    {
        auto held = std::make_shared<http::response<http::string_body>>(std::move(res));
        m_response = held;
        const bool keep_alive = held->keep_alive();
        http::async_write(m_stream, *held,
                          [self = this->shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
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
        if constexpr (is_tls) {
            // close_notify, so the peer can tell a finished stream from a cut one. Best effort:
            // the socket is going away either way, and the handler only exists to keep this
            // object alive until the shutdown completes.
            beast::get_lowest_layer(m_stream).expires_after(idle_timeout);
            m_stream.async_shutdown([self = this->shared_from_this()](beast::error_code) {});
        } else {
            beast::error_code ignored;
            m_stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        }
    }

    Stream                                                 m_stream;
    beast::flat_buffer                                     m_buffer;
    std::optional<http::request_parser<http::string_body>> m_parser;
    std::shared_ptr<http::response<http::string_body>>     m_response;
    const Registry                                        &m_registry;
    const ServerInfo                                      &m_info;
    std::string                                            m_token;
    bool                                                   m_lan = false;
};

} // namespace

// A page in a browser can reach a port it is not supposed to, so one the user happens to have open
// could try to drive the slicer. It cannot attach an Authorization header cross-origin without a
// successful preflight, and no preflight is ever answered here - but refusing a foreign Origin
// outright means that never has to be reasoned about again.
//
// A native client sends no Origin at all, which is the case `lan` is built for: on the network
// there is no origin that should be talking to this listener from a browser, including the
// client's own loopback, so absent is the only acceptable answer.
bool origin_allowed(const std::string &origin, bool lan)
{
    if (origin.empty() || origin == "null")
        return true;
    if (lan)
        return false;

    std::string rest;
    for (const char *scheme : {"http://", "https://"}) {
        const std::size_t length = std::strlen(scheme);
        if (origin.size() > length && origin.compare(0, length, scheme) == 0) {
            rest = origin.substr(length);
            break;
        }
    }
    if (rest.empty())
        return false;

    // The host is compared whole, not by prefix. "http://localhost.example.com" begins with
    // "http://localhost" and is a name anyone can register, so a prefix test would hand a
    // stranger's page the same standing as a page served from this machine.
    const std::size_t host_end = rest.find_first_of(":/");
    const std::string host     = host_end == std::string::npos ? rest : rest.substr(0, host_end);
    return host == "127.0.0.1" || host == "localhost";
}

struct McpListener::Impl
{
    Registry        registry;
    ServerInfo      info;
    ListenerOptions options;
    std::string     token = make_bearer_token();

    net::io_context             ioc{1};
    tcp::acceptor               acceptor{ioc};
    std::optional<ssl::context> tls;
    std::thread                 thread;
    std::atomic<bool>           running{false};
    unsigned short              bound_port = 0;

    Impl(Registry r, ServerInfo i, ListenerOptions o)
        : registry(std::move(r)), info(std::move(i)), options(std::move(o))
    {}

    // Returns false if an identity was supplied that OpenSSL would not take.
    bool prepare_tls()
    {
        if (!options.serves_tls())
            return true;
        try {
            ssl::context context{ssl::context::tls_server};
            // Everything before TLS 1.2 is off: there is no legacy client to accommodate here, the
            // only clients are ones written against this server.
            context.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                                ssl::context::single_dh_use);
            context.use_certificate_chain(net::buffer(options.certificate_pem));
            context.use_private_key(net::buffer(options.private_key_pem), ssl::context::file_format::pem);
            tls.emplace(std::move(context));
            return true;
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "MCP listener could not load its TLS identity: " << e.what();
            return false;
        }
    }

    void do_accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                // operation_aborted is stop() closing the acceptor, which is not a failure.
                if (ec != net::error::operation_aborted)
                    BOOST_LOG_TRIVIAL(error) << "MCP accept failed: " << ec.message();
                return;
            }
            if (tls)
                std::make_shared<Session<TlsStream>>(TlsStream(std::move(socket), *tls), registry, info, token,
                                                     options.lan)
                    ->run();
            else
                std::make_shared<Session<PlainStream>>(PlainStream(std::move(socket)), registry, info, token,
                                                       options.lan)
                    ->run();
            do_accept();
        });
    }
};

McpListener::McpListener(Registry registry, ServerInfo info, ListenerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(registry), std::move(info), std::move(options)))
{}

McpListener::~McpListener() { stop(); }

unsigned short McpListener::start()
{
    if (m_impl->running)
        return m_impl->bound_port;

    // Plaintext on the network would put the bearer token on the wire for anyone to read, and that
    // token is full write access to the slicer. Refuse rather than serve it.
    if (m_impl->options.lan && !m_impl->options.serves_tls()) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener refused to serve the network without a TLS identity";
        return 0;
    }
    if (!m_impl->prepare_tls())
        return 0;
    if (m_impl->token.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener has no bearer token, so it will not serve";
        return 0;
    }

    beast::error_code ec;
    // An ephemeral port: a fixed one is a port a hostile local process can squat on before the
    // slicer starts, and the client has to read the port out of the handshake file either way.
    // SO_REUSEADDR is deliberately left unset - on Windows it would let another process bind the
    // same port out from under this one.
    const net::ip::address address = m_impl->options.lan ? net::ip::address(net::ip::address_v4::any())
                                                         : net::ip::make_address("127.0.0.1");
    const tcp::endpoint    endpoint{address, 0};

    m_impl->acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener could not open a socket: " << ec.message();
        return 0;
    }
    m_impl->acceptor.bind(endpoint, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP listener could not bind " << endpoint.address().to_string() << ": "
                                 << ec.message();
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

    Impl *impl     = m_impl.get();
    m_impl->thread = std::thread([impl] {
        set_current_thread_name("mcp_listener");
        impl->ioc.run();
    });

    BOOST_LOG_TRIVIAL(info) << "MCP listener on " << endpoint.address().to_string() << ":" << m_impl->bound_port
                            << (m_impl->tls ? " (TLS)" : "");
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

bool McpListener::serves_tls() const { return m_impl && m_impl->tls.has_value(); }

const std::string &McpListener::token() const { return m_impl->token; }

}}} // namespace Slic3r::GUI::mcp
