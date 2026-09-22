# esp_cf_tunnel

Experimental **0.3.2** native C ESP-IDF component for a direct, remotely managed
Cloudflare Tunnel. It owns IPv4 edge discovery, verified TLS, an HTTP/2 server,
registration/configuration and native request callbacks. Your application owns
networking, the clock, credential storage and authorization.

## Install

Copy this directory to `<your-project>/components/esp_cf_tunnel`, or use the Git
dependency in the [project README](https://github.com/frxbg/esp-cf-tunnel#installation).
Requires ESP-IDF **6.1.0**. Add `CONFIG_CF_TUNNEL_ESP_TRANSPORT=y` to your
`sdkconfig.defaults` and keep TLS receive records at 16384 bytes.

Include [`esp_cf_tunnel.h`](include/esp_cf_tunnel.h), fill `esp_cf_tunnel_config`,
and call `esp_cf_tunnel_init()` / `esp_cf_tunnel_start()`. See the
[integration guide](https://github.com/frxbg/esp-cf-tunnel/blob/main/docs/integration.md)
and [System Monitor example](https://github.com/frxbg/esp-cf-tunnel/tree/main/examples/system_monitor).

## Scope

- ESP32-S3 live-tested; ESP32-P4 compile-tested only.
- One connection, hostname and exact service label. Native backend only.
- Two/four local application slots, separate control/config contexts and a
  65536-byte nghttp2 allocation quota.
- No Wi-Fi driver dependency, credential provisioning, Access JWT validation,
  WebSockets, SSE, QUIC, WARP or general local HTTP proxy.
- Experimental protocol compatibility; full production/long-duration validation
  remains pending.

MIT for project code. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)
and the licenses under `third_party/` and `certs/`.
