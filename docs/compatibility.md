# Compatibility — 2026-09-22

This is an experimental single-connection connector, not complete cloudflared
feature parity. See [validation](validation.md) for evidence.

| Area | Current status |
|---|---|
| Credentials | Standard Base64 JSON token; canonical UUID, hex account, 32–128-byte secret. Fixed English diagnostics. Custom endpoint refused. Real supplied token accepted. |
| Cap'n Proto / RPC | Host and official Go RPC tested; live bootstrap/registration succeeds. One imported registration capability, one outstanding question. No promise pipelines or third-party capabilities. |
| DNS / TLS | IPv4 SRV then A, UDP resolver, verified Cloudflare roots and h2.cftunnel.com. Real ESP connection to TCP 7844. No IPv6, DNS TCP fallback, CNAME chasing or DNSSEC. |
| HTTP/2 | Upstream nghttp2 1.70.0 SERVER role; bounded allocator, partial I/O, separate control/config slots. Host and S3 live tested. |
| Remote configuration | Exact hostname and permitted service plus final 404, empty originRequest, optional WARP enabled=false. Unknown/path/JWT/options fail closed. Live version 2 applied. |
| Shared monitor | Flash assets and authenticated JSON API shared with HTTPD. Public GET/HEAD and selected POST endpoints, fixed-size request/response contexts. User confirmed public page and updating telemetry through Access. |
| Local administration | Password unlock, settings and GPIO/RGB through AP/LAN; regression passed. Remote password unlock and authenticated API enabled in 0.3.1 at user request. No Access JWT validation inside firmware. |
| 10 MiB streaming | Host source and sink verified byte for byte without body buffering; hardware test NOT RUN. |
| 2/4 slots, overload, cancellation | Host exercised; monitor selects 4 plus one control and one config. Inbound callback must consume the entire chunk or the stream is reset. Full deferred inbound backpressure pending. |
| Reconnect / stop | Runtime backoff/reconnect and bounded graceful stop implemented; observed recovery after TLS connection timeout. 100 hardware transport lifecycle cycles NOT RUN. |
| ESP32-S3 / ESP32-P4 | Transport-linked compile profiles pass with and without PSRAM. Only S3 YD profile has been flashed and live tested. P4 default >=3.1; pre-v3 and no-PSRAM runtime NOT RUN. |
| WS / SSE / arbitrary methods | NOT IMPLEMENTED in monitor backend. GET/HEAD plus the monitor's bounded authenticated JSON POST routes; no arbitrary body forwarding. |
| Local HTTP proxy / WARP / QUIC | NOT IMPLEMENTED; outside the current native backend slice. |
| TLS mock / long soak / full conformance | NOT RUN. H2 memory harness and official RPC server are not full TLS mock edge tests. |

The remote config parser validates even stale updates and retains the last
valid structure on rejection. The lifecycle then rejects application traffic
until a valid configuration is applied. Empty optional originRequest is accepted;
unknown security options are not silently discarded. Configuration acceptance
does not create public DNS records or Access policies.

Serialized response headers preserve duplicates and use Cloudflare's format;
monitor requests do not depend on user cookie/header forwarding. Generic header,
body, compression, redirect, cookie and HTTP-method interoperability still needs
an end-to-end matrix before exposing a general-purpose backend.

## Cloudflare dashboard health and version

The user observed `Degraded` and a blank version for the 0.3.1 connector.
Cloudflare's [tunnel status documentation](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/troubleshoot-tunnels/common-errors/)
describes Healthy as serving through four edge connections. The pinned official
client defaults `ha-connections` to 4. This port intentionally opens one edge
connection, so the observed Degraded state is consistent with reduced redundancy.
One connector process can have several edge connections. The monitor's `Online`
means registration plus valid configuration, not Cloudflare's HA health verdict.

The 0.3.1 monitor supplied `esp-monitor/0.3.1` as ClientInfo.version; `cf_rpc.c` writes
it to pointer field 2, matching the generated Go schema and our oracle test.
The blank dashboard value therefore does not prove that the firmware omits the
field. Dashboard formatting of the custom version is a hypothesis, not a verified
cause. Cloudflare's [connector details API](https://developers.cloudflare.com/api/resources/zero_trust/subresources/tunnels/subresources/cloudflared/subresources/connectors/methods/get/)
exposes the stored `version`; that account-side value has not been inspected.
No official cloudflared release number is fabricated. Four-connection support
needs separate RAM/lifecycle work; current free RAM cannot be assumed sufficient
by simply multiplying the existing connection objects.


## Hard reload follow-up (0.3.2)

The intermittent 1033 and missing CSS were actual connection/stream failures,
separate from the dashboard's HA Degraded state. The wire stream setting now
matches cloudflared; local app admission remains bounded (four in the monitor).
Ten public Ctrl+F5 reloads were user-confirmed successful, and device diagnostics
recorded no failures/refusals. See [decisions](decisions.md) and
[validation](validation.md) for the captured GOAWAY and test scope. This is a
short regression result, not a long-duration availability guarantee. A genuine
edge shutdown still requires reconnecting the single connection.
