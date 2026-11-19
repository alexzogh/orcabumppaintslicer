#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <fstream>
#include <string>

#include <boost/filesystem/operations.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "slic3r/GUI/MCP/McpIdentity.hpp"

#include "test_utils.hpp"

using Slic3r::GUI::mcp::Identity;
using Slic3r::GUI::mcp::fingerprint_of;
using Slic3r::GUI::mcp::generate_identity;
using Slic3r::GUI::mcp::load_or_create_identity;
using Slic3r::GUI::mcp::write_private_file;

namespace {

std::string read_file(const boost::filesystem::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    REQUIRE(bool(in));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The fingerprint computed the way a client computes it: parse the PEM, hash the DER. If this
// agreed with fingerprint_of() by sharing its code the test would prove nothing, so it goes
// through OpenSSL directly.
std::string independent_fingerprint(const std::string &certificate_pem)
{
    BIO *bio = BIO_new_mem_buf(certificate_pem.data(), static_cast<int>(certificate_pem.size()));
    REQUIRE(bio != nullptr);
    X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    REQUIRE(certificate != nullptr);

    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int  length                  = 0;
    const int     ok = X509_digest(certificate, EVP_sha256(), digest, &length);
    X509_free(certificate);
    REQUIRE(ok == 1);

    static const char hex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

const char *not_a_certificate = "-----BEGIN CERTIFICATE-----\nnot base64 at all\n-----END CERTIFICATE-----\n";

} // namespace

TEST_CASE("A generated identity carries a certificate, a key and a fingerprint", "[McpIdentity]")
{
    const Identity identity = generate_identity();

    REQUIRE(identity.valid());
    CHECK(identity.certificate_pem.find("-----BEGIN CERTIFICATE-----") == 0);
    CHECK(identity.private_key_pem.find("-----BEGIN PRIVATE KEY-----") == 0);
    // SHA-256 is 32 bytes, so 64 hex characters.
    CHECK(identity.fingerprint_sha256.size() == 64);
    CHECK(identity.fingerprint_sha256.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("The reported fingerprint is the one a client computes from the certificate", "[McpIdentity]")
{
    const Identity identity = generate_identity();
    REQUIRE(identity.valid());

    // The whole security model rests on this equality: the client is handed a fingerprint at
    // pairing time and compares it against the certificate the server later offers. If the two
    // were computed over different bytes, every connection would be refused.
    CHECK(identity.fingerprint_sha256 == independent_fingerprint(identity.certificate_pem));
}

TEST_CASE("Two generated identities differ", "[McpIdentity]")
{
    const Identity first  = generate_identity();
    const Identity second = generate_identity();

    REQUIRE(first.valid());
    REQUIRE(second.valid());
    CHECK(first.fingerprint_sha256 != second.fingerprint_sha256);
    CHECK(first.private_key_pem != second.private_key_pem);
}

TEST_CASE("A fingerprint is not reported for something that is not a certificate", "[McpIdentity]")
{
    CHECK(fingerprint_of("").empty());
    CHECK(fingerprint_of("hello").empty());
    CHECK(fingerprint_of(not_a_certificate).empty());
}

TEST_CASE("An identity is created on first use and stored", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");

    const Identity created = load_or_create_identity(dir.path());
    REQUIRE(created.valid());

    CHECK(boost::filesystem::exists(dir.path() / "mcp-cert.pem"));
    CHECK(boost::filesystem::exists(dir.path() / "mcp-key.pem"));
    CHECK(read_file(dir.path() / "mcp-cert.pem") == created.certificate_pem);
    CHECK(read_file(dir.path() / "mcp-key.pem") == created.private_key_pem);
}

TEST_CASE("A stored identity is reused rather than regenerated", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");

    const Identity first = load_or_create_identity(dir.path());
    REQUIRE(first.valid());
    const Identity second = load_or_create_identity(dir.path());
    REQUIRE(second.valid());

    // Regenerating would unpair every client that had been told the old fingerprint, so this is
    // the property that makes pairing durable across restarts.
    CHECK(first.fingerprint_sha256 == second.fingerprint_sha256);
    CHECK(first.certificate_pem == second.certificate_pem);
    CHECK(first.private_key_pem == second.private_key_pem);
}

TEST_CASE("A stored certificate that cannot be parsed is replaced", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");

    const Identity first = load_or_create_identity(dir.path());
    REQUIRE(first.valid());

    {
        std::ofstream out((dir.path() / "mcp-cert.pem").string(), std::ios::binary | std::ios::trunc);
        out << not_a_certificate;
    }

    const Identity second = load_or_create_identity(dir.path());
    REQUIRE(second.valid());
    CHECK(second.fingerprint_sha256 != first.fingerprint_sha256);
    // The replacement is on disk, not just in memory: otherwise the next start would generate
    // another one and the client would be unpaired again.
    CHECK(read_file(dir.path() / "mcp-cert.pem") == second.certificate_pem);
}

TEST_CASE("A missing key is treated as no identity at all", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");

    const Identity first = load_or_create_identity(dir.path());
    REQUIRE(first.valid());

    boost::system::error_code ec;
    boost::filesystem::remove(dir.path() / "mcp-key.pem", ec);

    // A certificate without its key cannot complete a handshake, so keeping it would leave the
    // listener advertising an identity it cannot prove.
    const Identity second = load_or_create_identity(dir.path());
    REQUIRE(second.valid());
    CHECK(second.fingerprint_sha256 != first.fingerprint_sha256);
    CHECK(boost::filesystem::exists(dir.path() / "mcp-key.pem"));
}

TEST_CASE("A private file holds exactly what was written to it", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");
    const boost::filesystem::path path = dir.path() / "secret.txt";

    REQUIRE(write_private_file(path, "first"));
    CHECK(read_file(path) == "first");

    // Rewriting replaces rather than appends or leaves a tail of the longer previous contents.
    REQUIRE(write_private_file(path, "second value, longer"));
    CHECK(read_file(path) == "second value, longer");
    REQUIRE(write_private_file(path, "third"));
    CHECK(read_file(path) == "third");
}

TEST_CASE("An identity is not reported when it cannot be stored", "[McpIdentity]")
{
    const ScopedTemporaryDir dir("orca-mcp-identity");
    const boost::filesystem::path missing = dir.path() / "no-such-directory";

    // Nothing created the subdirectory, so neither file can be written. Reporting a valid
    // identity here would mean serving one that changes on every restart.
    const Identity identity = load_or_create_identity(missing);
    CHECK_FALSE(identity.valid());
}
