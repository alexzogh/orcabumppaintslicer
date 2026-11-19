# MCP over the network

The MCP server is reachable from another device — a tablet, a phone — over TLS, with the client
pinning the slicer's certificate rather than trusting a certificate authority. A person carries the
connection details across by scanning a QR code.

This exists because the tools the MCP server already offers describe a whole workflow — import,
arrange, orient, paint, change settings, slice, export — and a client that can call them does not
need to be the slicer. It needs to be able to reach it.

## Two listeners, not one

The service runs up to two listeners, and they are separate objects with separate bearer tokens.

| | loopback | network |
|---|---|---|
| bound to | `127.0.0.1` | every IPv4 interface |
| transport | plain HTTP | TLS 1.2+ |
| enabled by | `ORCA_MCP` | `ORCA_MCP_LAN`, in addition |
| token reaches the client via | the handshake file | a QR code |
| `Origin` accepted | absent, or a loopback origin | absent only |

They are separate because the threats are. Anything that can open the loopback port is already
running as this user and could read the handshake file anyway, so plain HTTP concedes nothing there
— and requiring TLS would break every local MCP client, none of which is prepared to trust a
private certificate. Anything that can route to this machine can open the network port, so on that
one the token must not cross the wire readable: it is full write access to the slicer, and
`export_gcode` and `run_app_action` are among the things it grants.

Separate tokens mean the two can be revoked independently. Restarting with a new token to lock out a
phone does not break a local client, which rereads the file.

The network listener refuses to start without a certificate and key rather than falling back to
plaintext. A control channel this broad is not something to serve in the clear because a file
failed to load.

## The identity

A self-signed P-256 certificate, generated on first use and kept in the data directory as
`mcp-cert.pem` and `mcp-key.pem`. The key is written owner-readable before anything is put in it.

It is self-signed because there is nothing a public authority could vouch for. The slicer answers on
whatever address the machine has on the network this morning; no CA issues certificates for
`192.168.1.20`. Pinning is also the stronger guarantee for this shape of problem — it admits exactly
one certificate, where a CA chain admits every certificate any CA has ever issued.

The consequences follow from that:

- The fingerprint must reach the client over a channel the network cannot touch, which is what the
  QR code is for.
- Regenerating the identity unpairs every client, so it is generated once and kept. It is replaced
  only when it cannot be parsed, when its key has gone missing, or when it is within a month of
  expiry, and it is issued for ten years so that is rare.
- The certificate names `orcaslicer.local` and `127.0.0.1`, deliberately not the machine's address
  on the network. That address changes, and a certificate naming it would need reissuing when it
  did, unpairing every client each time. Clients verify the fingerprint, not the name.

## Pairing

```
orcaslicer://pair?host=192.168.1.20&port=51820&token=<64 hex>&fp=<64 hex>
```

Everything the client needs and nothing it could look up: where to connect, the token to present,
and the one certificate to accept. No percent-encoding is involved, since every field is an IPv4
literal or lowercase hex.

`fp` is SHA-256 over the certificate's DER encoding — what every TLS stack means by "the certificate
fingerprint", so the value the client computes from the certificate it is offered matches this one
without either side agreeing on anything further.

The same values appear in the handshake file, `mcp.json` in the data directory, under `lan`:

```json
{
  "port": 49832,
  "token": "…",
  "url": "http://127.0.0.1:49832/mcp",
  "lan": {
    "port": 51820,
    "token": "…",
    "host": "192.168.1.20",
    "url": "https://192.168.1.20:51820/mcp",
    "fingerprint_sha256": "…",
    "pairing_uri": "orcaslicer://pair?…"
  }
}
```

## What a client has to do

1. Read the pairing URI, and keep `host`, `port`, `token` and `fp`.
2. Connect to `https://host:port/mcp` and, during the TLS handshake, compute SHA-256 over the DER of
   the certificate offered. Accept the connection only if it equals `fp`. Do not fall back to
   ordinary chain or hostname validation: the certificate will fail both, and it is meant to.
3. `POST` JSON-RPC to `/mcp` with `Authorization: Bearer <token>`, and no `Origin` header. Any other
   path, method, or an `Origin` of any value, is refused.
4. Treat a `202` with an empty body as the correct answer to a notification, not a truncated reply.

Because the certificate is pinned and never changes, a client that stores `fp` at pairing time keeps
working across slicer restarts, address changes and network moves. Only the `host` and `port` can go
stale, since the port is ephemeral and chosen afresh on each start.

## What shapes it

- **The port is ephemeral**, so a client that returns after the slicer restarted will not find it at
  the old one. A fixed port is worse: it is one a hostile local process can hold before the slicer
  starts.
- **IPv4 only.** A dual-stack socket is a separate decision, and nothing that pairs over a QR code on
  a home network has needed it.
- **The address is advertised, not discovered.** There is no mDNS record; the machine's address is
  taken as the first non-loopback IPv4 address the resolver reports for its host name. A machine
  with a VPN, a virtualisation bridge or a second adapter may well advertise the wrong one, and
  nothing in the slicer can tell which network the tablet is on.
- **The bearer token comes from OpenSSL's `RAND_bytes`**, not `std::random_device`, which the
  standard permits to be deterministic and which is exactly that in libstdc++ on mingw. A build made
  with it would otherwise hand every installation the same token.
