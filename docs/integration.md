# Integrating esp_cf_tunnel

## Install and configure

Use the Git dependency from the [README](../README.md#installation), or copy the
complete `components/esp_cf_tunnel` directory. Do not copy only its headers:
the vendored libraries and public edge CA roots are required.

Add the component to your application's requirements when using a local copy:

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES esp_cf_tunnel)
```

The transport is opt-in in `sdkconfig.defaults`:

```ini
CONFIG_CF_TUNNEL_ESP_TRANSPORT=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
```

The component targets ESP-IDF 6.1.0, ESP32-S3 and ESP32-P4. It does not initialize
Wi-Fi, Ethernet, esp_netif, event loops, SNTP or NVS. P4 applications must supply
their own working network interface. The monitor's Wi-Fi/GPIO code is a separate
S3 example, not part of the library.

The native mbedTLS transport now enables TLS 1.3 (certificate and hostname
verification remain mandatory). The tested edge rejected TLS 1.2 with alert 70.
For the pinned IDF v6.1, explicitly prepare the [guarded TCP poll override](../patches/esp-idf-v6.1/README.md)
and add its component directory to `EXTRA_COMPONENT_DIRS` before IDF project
initialization. This SDK fix is separate from the component package.

Retained `last_connect` and `last_connect_failure` snapshots distinguish TCP,
TLS setup/handshake and application deadline. Numeric ESP/TLS/verification/system
errors are captured before teardown. `errno_context` can be stale even on success;
`utc_s` is wall time, while elapsed/deadline use monotonic time. See the
[measured diagnosis](tcp-tls-diagnostics.md).

## Application contract

`esp_cf_tunnel_config` is defined in
[`esp_cf_tunnel.h`](../components/esp_cf_tunnel/include/esp_cf_tunnel.h).

| Callback / field | Responsibility |
|---|---|
| `context` | Application state; valid until deinit completes |
| `environment` | Copy network/clock readiness and four-byte local/DNS IPv4 addresses |
| `credentials` | Read a provisioned token, parse with `cf_credentials_parse`, copy the configured hostname, then wipe temporary secrets |
| `json_try_lock` / `json_unlock` | Nonblocking serialization of the private JSON parser with any application users of it |
| `request` | Accept native HTTP headers and allocate/find a bounded per-stream context |
| `data` | Consume/copy the complete chunk; `end_stream` marks the end of the request |
| `read_response` | Fill at most the supplied capacity; set length and EOF |
| `closed` | Release/wipe the context on normal completion, cancellation or teardown |
| `version` / `arch` | Your client identification strings, each 1-63 bytes |
| `service` | Exact native backend label, for example `http://localhost:80` |
| `application_streams` | Exactly 2 or 4; monitor uses 4 |

All transport callbacks run in one owner task. Keep them nonblocking: a delayed
callback delays every request and the control stream. Callback context and the
configuration's string storage must outlive the connector. The init function
copies the configuration structure, not the strings it points to.

The credential callback runs with your JSON lock held. Use a buffer of
`CF_TOKEN_JSON_MAX + 1` for `cf_credentials_parse`, and wipe the token/scratch
with `cf_secure_zero`. The transport wipes the parsed credentials after encoding
registration. Do not put tokens in source, compiler definitions or logs.

## Start, inspect, reload and stop

After defining callbacks according to the table, construct the configuration
and retain the returned handle:

```c
esp_cf_tunnel *tunnel = NULL;
ESP_ERROR_CHECK(esp_cf_tunnel_init(&config, &tunnel));
ESP_ERROR_CHECK(esp_cf_tunnel_start(tunnel));

esp_cf_tunnel_snapshot status;
esp_cf_tunnel_get_snapshot(tunnel, &status);
// status.state == CF_ONLINE requires both registration and valid ingress.
```

This excerpt assumes `config` is populated by your integration; the complete
[monitor adapter](../examples/system_monitor/main/monitor_tunnel.c) demonstrates
every callback. The [protocol smoke](../examples/protocol_smoke) example is a
network-free lifecycle/compile check, not a live provisioning example.

- `esp_cf_tunnel_reload()` asynchronously reloads saved credentials and restarts
  the connection. Coordinate application state updates before calling it.
- `esp_cf_tunnel_stop()` requests asynchronous shutdown. Call
  `esp_cf_tunnel_deinit()` later; it returns `ESP_ERR_INVALID_STATE` until the
  owner task has stopped. Keep the handle/context alive until it returns `ESP_OK`.
- Snapshots are synchronized copies. Last-failure text/count/time survive a
  reconnect, but are in RAM and reset with the connector/board.

## Native response lifecycle

1. In `request`, inspect `cf_h2_request` and use `cf_h2_header()` for raw headers.
   Header spans borrow session scratch and are valid only inside the callback.
2. For a body-dependent handler, wait for `data(..., end_stream=true)` before
   executing a mutation. Check method, length, origin and authorization in your
   application; the generic component does not implement those policies for you.
3. Call `cf_h2_respond(h2, stream_id, status, headers, count, true)` once. The
   library serializes origin headers for Cloudflare and copies them for sending.
4. `read_response` streams bytes. Return `CF_ERR_STATE` to defer outgoing data;
   later call `cf_h2_resume()` from the owner context. No other task may call H2
   functions directly; communicate with the owner through application state.
5. `closed` ends ownership even if a request failed. Release all per-stream data.

Incoming chunks are at most 2048 bytes. They must be consumed completely on
return; an error resets that stream. Full deferred inbound backpressure is not
implemented. Whole uploads/downloads are not buffered by the component.
The monitor limits its JSON bodies to 2048 bytes and leaves static assets in flash.

Wire concurrency matches cloudflared; local admission is still restricted to
the configured application slots plus one control and one config slot. Excess
streams are refused, and the nghttp2 allocator has a hard quota. Do not interpret
the advertised setting as permission to allocate an unbounded application queue.

## Cloudflare configuration and authorization

Create a named, remotely managed tunnel and supply its connector-install token.
Configure exactly one hostname with a service matching `config.service`, followed
by `http_status:404`. The supported configuration has empty origin options and
optional WARP `enabled: false`. Unsupported options are rejected explicitly.

The monitor example uses `device.example.com` as a documentation placeholder and
`http://localhost:80` as the native handler label. It does not proxy TCP to
localhost. Public DNS and Access policies are configured in Cloudflare.

TLS authenticates the edge, not your end user. Protect administrative routes
with application authentication. Never trust a locally supplied Access header
as an authenticated user identity. See [security](security.md),
[compatibility](compatibility.md) and [memory bounds](memory-budget.md).
