# Security policy

This is experimental software. The current maintained version is 0.3.2; no
production security audit or long-duration availability claim is made.

Use [GitHub private vulnerability reporting](https://github.com/frxbg/esp-cf-tunnel/security/advisories/new)
for security issues. Do not put credentials, device dumps or exploit details
containing private data in public issues. Provide a synthetic reproducer when possible.

The library verifies TLS and requires a valid clock. Configure Cloudflare Access
separately; the firmware does not validate Access JWTs. Applications must
authenticate sensitive handlers. The monitor uses its own device password and
temporary bearer session on both local and tunnel routes.

The development monitor stores credentials in unencrypted NVS and serves local
HTTP. Restrict provisioning to a trusted network. Production encryption, secure
boot and physical protection are application/deployment decisions. No source
password, configured NVS image or private key is distributed.

See [the security model](docs/security.md) for the implemented boundaries.
