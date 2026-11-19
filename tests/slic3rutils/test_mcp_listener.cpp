#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <openssl/crypto.h>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "slic3r/GUI/MCP/McpIdentity.hpp"
#include "slic3r/GUI/MCP/McpListener.hpp"
#include "slic3r/GUI/MCP/McpProtocol.hpp"
#include "slic3r/GUI/MCP/McpService.hpp"

using Slic3r::GUI::mcp::Identity;
using Slic3r::GUI::mcp::ListenerOptions;
using Slic3r::GUI::mcp::McpListener;
using Slic3r::GUI::mcp::Registry;
using Slic3r::GUI::mcp::ServerInfo;
using Slic3r::GUI::mcp::generate_identity;
using Slic3r::GUI::mcp::make_bearer_token;
using Slic3r::GUI::mcp::make_pairing_uri;
using Slic3r::GUI::mcp::origin_allowed;

namespace {

ServerInfo test_info()
{
    ServerInfo info;
    info.name    = "test";
    info.version = "0.0.0";
    return info;
}

} // namespace

TEST_CASE("A listener will not serve the network without a TLS identity", "[McpListener]")
{
    ListenerOptions options;
    options.lan = true;
    REQUIRE_FALSE(options.serves_tls());

    McpListener listener(Registry{}, test_info(), options);

    // Plaintext on the network would put the bearer token - full write access to the slicer - on
    // the wire for anyone to read. Refusing is the whole point; falling back would be worse than
    // not running at all. Nothing is bound, so this neither opens a port nor needs one.
    CHECK(listener.start() == 0);
    CHECK_FALSE(listener.is_running());
}

TEST_CASE("Listener options report whether they carry a usable identity", "[McpListener]")
{
    ListenerOptions options;
    CHECK_FALSE(options.serves_tls());

    // Half an identity is not an identity: a certificate cannot be served without its key, and a
    // key proves nothing without the certificate naming it.
    options.certificate_pem = "cert";
    CHECK_FALSE(options.serves_tls());
    options.certificate_pem.clear();
    options.private_key_pem = "key";
    CHECK_FALSE(options.serves_tls());

    options.certificate_pem = "cert";
    CHECK(options.serves_tls());
}

TEST_CASE("A loopback listener admits only same-machine origins", "[McpListener]")
{
    const bool lan = false;

    // A native client sends no Origin at all.
    CHECK(origin_allowed("", lan));
    CHECK(origin_allowed("null", lan));

    CHECK(origin_allowed("http://127.0.0.1:8080", lan));
    CHECK(origin_allowed("http://localhost:3000", lan));
    CHECK(origin_allowed("https://127.0.0.1", lan));

    CHECK_FALSE(origin_allowed("http://example.com", lan));
    CHECK_FALSE(origin_allowed("https://evil.example", lan));

    // A host that merely begins with an allowed one is a different host, and both of these are
    // names a stranger can register and serve a page from.
    CHECK_FALSE(origin_allowed("http://localhost.example.com", lan));
    CHECK_FALSE(origin_allowed("https://127.0.0.1.example.com", lan));

    // Nor is an allowed host buried somewhere else in the string.
    CHECK_FALSE(origin_allowed("http://example.com/localhost", lan));
    CHECK_FALSE(origin_allowed("http://user@evil.example", lan));
    CHECK_FALSE(origin_allowed("localhost", lan));
    CHECK_FALSE(origin_allowed("ftp://localhost", lan));
}

TEST_CASE("A network listener admits no browser origin at all", "[McpListener]")
{
    const bool lan = true;

    // Native clients, which is everything this listener exists for.
    CHECK(origin_allowed("", lan));
    CHECK(origin_allowed("null", lan));

    // On the network there is no origin that should be reaching this listener from a browser -
    // including the client machine's own loopback, which says nothing about who is asking.
    CHECK_FALSE(origin_allowed("http://127.0.0.1:8080", lan));
    CHECK_FALSE(origin_allowed("http://localhost:3000", lan));
    CHECK_FALSE(origin_allowed("https://example.com", lan));
}

TEST_CASE("A bearer token is 256 bits of hex and does not repeat", "[McpListener]")
{
    const std::string first  = make_bearer_token();
    const std::string second = make_bearer_token();

    REQUIRE(first.size() == 64);
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    // Two tokens drawn in a row being equal would mean the source is not random at all, which is
    // precisely the mingw std::random_device behaviour this function exists to avoid.
    CHECK(first != second);
}

TEST_CASE("A pairing URI carries the address, the token and the fingerprint", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    const std::string uri = make_pairing_uri("192.168.1.20", 51820, "abc123", identity.fingerprint_sha256);

    CHECK(uri == "orcaslicer://pair?host=192.168.1.20&port=51820&token=abc123&fp=" + identity.fingerprint_sha256);
}

TEST_CASE("A pairing URI pins the exact certificate the client will be offered", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    const std::string uri = make_pairing_uri("10.0.0.5", 1024, make_bearer_token(), identity.fingerprint_sha256);

    // The fingerprint has to survive into the URI unaltered, since the client compares it byte for
    // byte against what the TLS handshake offers. A truncated or re-cased value refuses every
    // connection, and the failure would look like a network fault rather than a formatting bug.
    CHECK(uri.find("&fp=" + identity.fingerprint_sha256) != std::string::npos);
    CHECK(identity.fingerprint_sha256.size() == 64);
}

// --- A real connection ------------------------------------------------------------------------
//
// Everything above tests a rule in isolation. These drive an actual listener over an actual TLS
// handshake on loopback, because the thing most likely to be wrong is not a predicate but whether
// the certificate OpenSSL was handed can complete a handshake at all - and no amount of asserting
// on strings finds that out.

namespace {

struct Reply
{
    int         status = 0;
    std::string body;
};

std::string peer_fingerprint(SSL *ssl)
{
    X509 *certificate = SSL_get1_peer_certificate(ssl);
    if (certificate == nullptr)
        return {};

    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int  length                  = 0;
    const int     ok                      = X509_digest(certificate, EVP_sha256(), digest, &length);
    X509_free(certificate);
    if (ok != 1)
        return {};

    static const char hex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

// A paired client in miniature: connect, pin the certificate, present the token, send one request.
// `fingerprint_seen` reports what the server actually offered so the caller can assert on it.
Reply request_over_tls(unsigned short     port,
                       const std::string &token,
                       const std::string &body,
                       std::string       &fingerprint_seen,
                       const std::string &origin = {})
{
    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace net   = boost::asio;
    namespace ssl   = net::ssl;
    using tcp       = net::ip::tcp;

    net::io_context ioc;
    // verify_none, then pin by hand: a self-signed certificate naming no address fails chain and
    // hostname validation by design, and the fingerprint is what stands in for both.
    ssl::context context{ssl::context::tls_client};
    context.set_verify_mode(ssl::verify_none);

    beast::ssl_stream<beast::tcp_stream> stream{ioc, context};
    beast::get_lowest_layer(stream).connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    stream.handshake(ssl::stream_base::client);

    fingerprint_seen = peer_fingerprint(stream.native_handle());

    http::request<http::string_body> req{http::verb::post, "/mcp", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    if (!token.empty())
        req.set(http::field::authorization, "Bearer " + token);
    if (!origin.empty())
        req.set(http::field::origin, origin);
    req.body() = body;
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer                buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ignored;
    stream.shutdown(ignored);

    return Reply{static_cast<int>(res.result_int()), res.body()};
}

// A TLS listener on loopback. The service only ever pairs TLS with the network, but the two are
// independent settings on the listener, and loopback is what a test can bind without asking a
// firewall for permission.
ListenerOptions tls_on_loopback(const Identity &identity)
{
    ListenerOptions options;
    options.lan             = false;
    options.certificate_pem = identity.certificate_pem;
    options.private_key_pem = identity.private_key_pem;
    return options;
}

const char *ping_request = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

} // namespace

TEST_CASE("A TLS listener completes a handshake and answers an authorised request", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    McpListener          listener(Registry{}, test_info(), tls_on_loopback(identity));
    const unsigned short port = listener.start();
    REQUIRE(port != 0);
    REQUIRE(listener.serves_tls());

    std::string  offered;
    const Reply  reply = request_over_tls(port, listener.token(), ping_request, offered);

    // The certificate the handshake offered is the one whose fingerprint pairing hands out. If
    // these ever differed, every paired client would refuse to connect and the cause would look
    // like a network fault.
    CHECK(offered == identity.fingerprint_sha256);
    CHECK(reply.status == 200);
    CHECK(reply.body.find("\"result\"") != std::string::npos);
}

TEST_CASE("A TLS listener refuses a request carrying the wrong token", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    McpListener          listener(Registry{}, test_info(), tls_on_loopback(identity));
    const unsigned short port = listener.start();
    REQUIRE(port != 0);

    std::string offered;

    // A token of the right shape but the wrong value: the comparison is constant-time, so this is
    // checking that it still says no.
    const Reply wrong = request_over_tls(port, make_bearer_token(), ping_request, offered);
    CHECK(wrong.status == 401);

    const Reply none = request_over_tls(port, "", ping_request, offered);
    CHECK(none.status == 401);
}

TEST_CASE("A TLS listener refuses a request from a foreign origin", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    McpListener          listener(Registry{}, test_info(), tls_on_loopback(identity));
    const unsigned short port = listener.start();
    REQUIRE(port != 0);

    std::string offered;

    // Refused before the token is even looked at, so a page that somehow obtained the token still
    // gets nowhere.
    const Reply foreign = request_over_tls(port, listener.token(), ping_request, offered, "https://evil.example");
    CHECK(foreign.status == 403);

    const Reply local = request_over_tls(port, listener.token(), ping_request, offered, "http://localhost:3000");
    CHECK(local.status == 200);
}

TEST_CASE("A stopped listener releases its port", "[McpListener]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    unsigned short port = 0;
    {
        McpListener listener(Registry{}, test_info(), tls_on_loopback(identity));
        port = listener.start();
        REQUIRE(port != 0);
        CHECK(listener.is_running());
        listener.stop();
        CHECK_FALSE(listener.is_running());
    }

    // Nothing is listening, so connecting is refused rather than hanging or being answered by a
    // listener that outlived its owner.
    namespace net = boost::asio;
    using tcp     = net::ip::tcp;
    net::io_context           ioc;
    tcp::socket               socket(ioc);
    boost::system::error_code ec;
    socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port), ec);
    CHECK(bool(ec));
}
