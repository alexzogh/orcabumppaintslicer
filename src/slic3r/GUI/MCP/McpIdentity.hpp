#ifndef slic3r_McpIdentity_hpp_
#define slic3r_McpIdentity_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI { namespace mcp {

// The TLS identity the MCP listener serves on the LAN.
//
// Self-signed and pinned, not chained to a certificate authority. There is no name a public CA
// could vouch for here - the slicer answers on whatever address the machine happens to have on the
// network this morning - so the client is told the exact certificate to expect at pairing time and
// refuses anything else. That is a stronger guarantee than a CA chain for this shape of problem:
// it admits exactly one certificate rather than every certificate any CA has ever issued.
//
// The consequence is that the fingerprint has to reach the client over a channel the network cannot
// touch, which is what the pairing QR code is for, and that regenerating the identity unpairs every
// client. So the identity is generated once and kept.
struct Identity
{
    std::string certificate_pem;
    std::string private_key_pem;
    // SHA-256 over the certificate's DER encoding - the same bytes every TLS stack calls "the
    // certificate fingerprint" - as lowercase hex with no separators.
    std::string fingerprint_sha256;

    bool valid() const { return !certificate_pem.empty() && !private_key_pem.empty(); }
};

// Reads the identity from `dir`, generating and storing one if there is none, if it cannot be
// parsed, or if it has expired. Returns an invalid Identity if it could neither read nor create
// one, which the caller must treat as "do not serve on the LAN".
Identity load_or_create_identity(const boost::filesystem::path &dir);

// A fresh self-signed identity, generated in memory. Exposed for tests, and for callers that want
// to regenerate deliberately rather than on expiry.
Identity generate_identity();

// The SHA-256 fingerprint of a PEM certificate, lowercase hex, or an empty string if the PEM does
// not parse.
std::string fingerprint_of(const std::string &certificate_pem);

// A 256-bit bearer token as lowercase hex. Empty when the system's random source failed, which the
// caller must treat as "do not serve" rather than falling back to something weaker.
//
// Not std::random_device: the standard permits it to be a deterministic engine, and libstdc++ on
// mingw is exactly that, so a build made with it would hand every installation the same token. On
// loopback that is survivable; on the network it is the whole of the authentication. OpenSSL is
// already linked here and RAND_bytes is a CSPRNG on every platform this builds for.
std::string make_bearer_token();

// Write `contents` to `path` so that only this user can read it.
//
// The file carries a secret - a private key, or the bearer token - so it is created empty and
// narrowed before anything is written into it: setting the permissions afterwards would leave a
// window in which the secret sits in a world-readable file. An existing file is removed rather
// than truncated, since truncating keeps whatever permissions it already had.
//
// owner_read | owner_write is POSIX. On Windows boost maps this onto the read-only attribute
// rather than a real ACL, so there the per-user data directory stays the whole of the protection.
bool write_private_file(const boost::filesystem::path &path, const std::string &contents);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpIdentity_hpp_
