# Security model

## Credentials and storage

Applications supply provisioned tokens through a callback; there is no built-in
token or shared device password. Parser scratch, decoded credentials and queued
RPC bytes are wiped after use. Native status and diagnostics do not return the
token, password, session or raw request/peer debug strings.

The monitor's provisioning utility generates a unique random password in a
Git-ignored local directory. NVS and local HTTP are unencrypted development
interfaces. Protect the provisioning network and physical device. Production
flash/NVS encryption and secure boot require board-specific deployment work;
the library does not change eFuses or silently erase NVS.

## Transport

TLS verifies the certificate chain, `h2.cftunnel.com` identity and certificate
validity. A valid clock is required; there is no insecure fallback. The three
public Cloudflare edge roots are bundled and must be maintained when upstream
trust changes. Application chunks are not a reason to shrink TLS records below
the supported 16384-byte receive size.

DNS selects an allowed Cloudflare target suffix on TCP 7844. A malicious DNS
answer cannot bypass TLS validation. The native service label does not create
an arbitrary outbound origin connection or LAN proxy.

Remote configuration supports an explicit subset. Unsupported JWT/origin/WARP
options are rejected, not dropped. Unknown configuration values are not logged.
The prior valid configuration is retained when an update is rejected.

## Application authorization

Cloudflare Tunnel does not configure Access policy or authenticate end users for
the application. The firmware does not validate Access JWTs. Never treat a local
HTTP header as a trusted Access identity. Implement authentication for all
sensitive native handlers.

The monitor uses a constant-time device-password comparison, five failed login
attempts per minute and a random ten-minute bearer session. The browser keeps the
session in memory. A new login invalidates the old session. Both local and remote
routes share authorization/validation and have no CORS grant or cookie-based
device authentication.

Local Host/Origin must match the device IP. Remote authority must match the
configured hostname; a supplied Origin must be its HTTPS origin. Non-browser
clients without Origin still need the session. Ambiguous authorization, content
type, origin and length headers are rejected, including raw/serialized duplicates.

## Resource bounds and mutations

The component owns one transport task. Fixed request/header/RPC/config limits,
the 65536-byte nghttp2 quota and the 32768-byte private JSON quota limit allocation.
The large advertised wire stream setting does not remove local admission limits.
Excess requests are refused; allocation/protocol failures can close the connection.
This is not a claim of complete denial-of-service resistance.

The monitor waits for complete bounded JSON input before a mutation, rejects
oversized or ambiguous requests and wipes request/response buffers on reset.
It does not automatically replay mutations after reconnect. NVS commit may
briefly block its API; an asynchronous save worker remains future work.
Generic handlers must implement their own equivalent protections.

Report vulnerabilities through [SECURITY.md](../SECURITY.md). Tests and sanitizer
results cover the documented subset, not a complete security audit.
