#include "McpIdentity.hpp"

#include <array>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

constexpr const char *certificate_file = "mcp-cert.pem";
constexpr const char *private_key_file = "mcp-key.pem";

// Long, because the client pins this certificate: an expiry date is a day on which every paired
// client stops working at once, and re-pairing is a manual act involving a QR code and a person.
// Pinning rather than the validity window is what makes this certificate trustworthy, so the
// window only has to be long enough to stay out of the way.
constexpr long certificate_lifetime_seconds = 10L * 365 * 24 * 60 * 60;

// Replace it while it still has a month to run, so the reissue lands between sessions rather than
// in the middle of one.
constexpr long renew_when_less_than_seconds = 30L * 24 * 60 * 60;

struct BioDeleter { void operator()(BIO *p) const { if (p) BIO_free(p); } };
struct X509Deleter { void operator()(X509 *p) const { if (p) X509_free(p); } };
struct PkeyDeleter { void operator()(EVP_PKEY *p) const { if (p) EVP_PKEY_free(p); } };
struct BnDeleter { void operator()(BIGNUM *p) const { if (p) BN_free(p); } };
struct ExtDeleter { void operator()(X509_EXTENSION *p) const { if (p) X509_EXTENSION_free(p); } };

using BioPtr  = std::unique_ptr<BIO, BioDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using BnPtr   = std::unique_ptr<BIGNUM, BnDeleter>;
using ExtPtr  = std::unique_ptr<X509_EXTENSION, ExtDeleter>;

std::string bio_to_string(BIO *bio)
{
    char      *data   = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    if (length <= 0 || data == nullptr)
        return {};
    return std::string(data, static_cast<std::size_t>(length));
}

X509Ptr parse_certificate(const std::string &pem)
{
    if (pem.empty())
        return nullptr;
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio)
        return nullptr;
    return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

// A serial number has to be positive and should not be guessable; beyond that nothing here reads
// it, since there is no CA and no revocation list.
bool set_random_serial(X509 *certificate)
{
    unsigned char bytes[8];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1)
        return false;
    bytes[0] &= 0x7f; // clear the sign bit so the integer is positive

    BnPtr number(BN_bin2bn(bytes, static_cast<int>(sizeof(bytes)), nullptr));
    if (!number)
        return false;
    return BN_to_ASN1_INTEGER(number.get(), X509_get_serialNumber(certificate)) != nullptr;
}

bool add_extension(X509 *certificate, int nid, const char *value)
{
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);

    ExtPtr extension(X509V3_EXT_conf_nid(nullptr, &context, nid, value));
    if (!extension)
        return false;
    return X509_add_ext(certificate, extension.get(), -1) == 1;
}

std::string read_file(const boost::filesystem::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return in.good() || in.eof() ? buffer.str() : std::string();
}

// True when the certificate is missing, unreadable, or close enough to its expiry that it should
// be replaced now rather than later.
bool needs_replacing(const std::string &certificate_pem)
{
    const X509Ptr certificate = parse_certificate(certificate_pem);
    if (!certificate)
        return true;

    std::time_t deadline = std::time(nullptr);
    if (deadline == static_cast<std::time_t>(-1))
        return true;
    deadline += renew_when_less_than_seconds;

    // -1 means the certificate's notAfter is earlier than the deadline; 0 means the comparison
    // failed, which is its own reason to replace it.
    return X509_cmp_time(X509_get0_notAfter(certificate.get()), &deadline) != 1;
}

} // namespace

std::string fingerprint_of(const std::string &certificate_pem)
{
    const X509Ptr certificate = parse_certificate(certificate_pem);
    if (!certificate)
        return {};

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int                               length = 0;
    // X509_digest hashes the DER encoding, which is what every TLS stack means by "the certificate
    // fingerprint" - so the value a client computes from the certificate it is offered matches
    // this one without either side having to agree on anything further.
    if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &length) != 1 || length == 0)
        return {};

    static const char hex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

std::string make_bearer_token()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
        BOOST_LOG_TRIVIAL(error) << "MCP could not draw a token from the system random source";
        return {};
    }

    static const char hex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(sizeof(bytes) * 2);
    for (unsigned char byte : bytes) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

Identity generate_identity()
{
    // P-256 rather than RSA: every TLS stack that matters supports it, iOS included, and the key,
    // the certificate and the handshake are all smaller for equivalent strength.
    PkeyPtr key(EVP_EC_gen("prime256v1"));
    if (!key) {
        BOOST_LOG_TRIVIAL(error) << "MCP identity: could not generate a P-256 key";
        return {};
    }

    X509Ptr certificate(X509_new());
    if (!certificate)
        return {};

    // 2 is the encoding of X.509 v3, which is what the extensions below require.
    if (X509_set_version(certificate.get(), 2) != 1 || !set_random_serial(certificate.get()))
        return {};

    // An hour of slack before it is valid, so a client whose clock runs slightly behind the
    // slicer's does not reject a certificate generated moments ago.
    if (X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), certificate_lifetime_seconds) == nullptr)
        return {};

    if (X509_set_pubkey(certificate.get(), key.get()) != 1)
        return {};

    X509_NAME *name = X509_get_subject_name(certificate.get());
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("OrcaSlicer MCP"), -1, -1, 0) != 1 ||
        X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("OrcaSlicer"), -1, -1, 0) != 1)
        return {};
    // Self-signed, so the issuer is the subject.
    if (X509_set_issuer_name(certificate.get(), name) != 1)
        return {};

    // A client that pins the fingerprint never reads these. They are here because a TLS stack may
    // insist on a well-formed leaf certificate before it hands the decision to the application,
    // and a certificate that is refused before pinning is consulted is refused for good.
    if (!add_extension(certificate.get(), NID_basic_constraints, "critical,CA:FALSE") ||
        !add_extension(certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
        !add_extension(certificate.get(), NID_ext_key_usage, "serverAuth"))
        return {};

    // Deliberately not the machine's LAN address. That address changes - a new lease, a different
    // network - and a certificate naming it would have to be reissued when it did, which would
    // unpair every client each time. The names here are stable and the fingerprint is what the
    // client actually checks.
    if (!add_extension(certificate.get(), NID_subject_alt_name, "DNS:orcaslicer.local,IP:127.0.0.1"))
        return {};

    if (X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0) {
        BOOST_LOG_TRIVIAL(error) << "MCP identity: could not sign the certificate";
        return {};
    }

    BioPtr certificate_bio(BIO_new(BIO_s_mem()));
    if (!certificate_bio || PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1)
        return {};

    BioPtr key_bio(BIO_new(BIO_s_mem()));
    if (!key_bio ||
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return {};

    Identity identity;
    identity.certificate_pem    = bio_to_string(certificate_bio.get());
    identity.private_key_pem    = bio_to_string(key_bio.get());
    identity.fingerprint_sha256 = fingerprint_of(identity.certificate_pem);
    if (!identity.valid() || identity.fingerprint_sha256.empty())
        return {};
    return identity;
}

bool write_private_file(const boost::filesystem::path &path, const std::string &contents)
{
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);

    {
        std::ofstream create(path.string(), std::ios::binary | std::ios::trunc);
        if (!create)
            return false;
    }

    boost::filesystem::permissions(path, boost::filesystem::owner_read | boost::filesystem::owner_write, ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "MCP could not narrow " << path.string() << ": " << ec.message();

    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << contents;
    out.flush();
    return bool(out);
}

Identity load_or_create_identity(const boost::filesystem::path &dir)
{
    const boost::filesystem::path certificate_path = dir / certificate_file;
    const boost::filesystem::path key_path         = dir / private_key_file;

    Identity stored;
    stored.certificate_pem = read_file(certificate_path);
    stored.private_key_pem = read_file(key_path);
    if (stored.valid() && !needs_replacing(stored.certificate_pem)) {
        stored.fingerprint_sha256 = fingerprint_of(stored.certificate_pem);
        if (!stored.fingerprint_sha256.empty())
            return stored;
    }
    if (stored.valid())
        BOOST_LOG_TRIVIAL(info) << "MCP identity at " << certificate_path.string()
                                << " is expiring or unreadable; generating a new one. Paired clients "
                                   "will have to be paired again.";

    const Identity fresh = generate_identity();
    if (!fresh.valid()) {
        BOOST_LOG_TRIVIAL(error) << "MCP identity: could not generate one";
        return {};
    }

    // An identity that is not on disk would be regenerated on the next start, so every client
    // would pair once and break on restart. Better to report that the LAN listener cannot run.
    bool written = write_private_file(key_path, fresh.private_key_pem);
    if (written) {
        std::ofstream out(certificate_path.string(), std::ios::binary | std::ios::trunc);
        out << fresh.certificate_pem;
        out.flush();
        written = bool(out);
    }
    if (!written) {
        BOOST_LOG_TRIVIAL(error) << "MCP identity: could not store one in " << dir.string();
        boost::system::error_code ignored;
        boost::filesystem::remove(key_path, ignored);
        boost::filesystem::remove(certificate_path, ignored);
        return {};
    }

    return fresh;
}

}}} // namespace Slic3r::GUI::mcp
