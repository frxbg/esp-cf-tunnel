# Protocol reference

This port follows cloudflared 2026.9.1, commit
`f11dea9cb7079e90a982c1a2d5548ab40847fdcf`. The internal tunnel protocol is not
a guaranteed stable public API. Other dependency pins are in
[dependencies.lock.json](../dependencies.lock.json).

## Connection roles

The ESP opens an outbound TCP connection to the discovered edge on port 7844,
then acts as a TLS client verifying `h2.cftunnel.com`. Within that connection,
the ESP acts as an **HTTP/2 server** and the edge opens request streams.

| Pinned upstream source | Role |
|---|---|
| [supervisor/tunnel.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/supervisor/tunnel.go) | HTTP2 connection creation and serving |
| [edgediscovery/dial.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/edgediscovery/dial.go) | Outgoing TCP and TLS client |
| [discovery.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/edgediscovery/allregions/discovery.go) | SRV discovery and addresses |
| [connection/protocol.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/connection/protocol.go) | Protocol TLS identity |
| [connection/http2.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/connection/http2.go) | Server role, stream classification, response/config mapping |
| [connection/connection.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/connection/connection.go) | Token representation and wire concurrency setting |
| [connection/control.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/connection/control.go) | Registration and unregister lifecycle |

Discovery uses `_v2-origintunneld._tcp.argotunnel.com`, then an IPv4 A query for
an allowed SRV target. DNS target and TLS identity are different. SRV priorities
rotate across retries, with weighting within equal-priority groups. This port
does not implement IPv6, CNAME traversal, DNS TCP/DoT fallback or caching.

The three public edge CA roots come from the pinned reference. HTTP/2 does not
require the QUIC ALPN settings. TLS authentication and a valid clock are mandatory.

## Credentials and headers

A named connector token is standard padded Base64 JSON: account tag `a`, Base64
secret `s`, UUID `t`, and optional endpoint `e`. The supported parser requires a
32-hex account, canonical nonzero UUID and 32-128 secret bytes. Nonempty custom
endpoints are rejected. An API key and an HTTP Bearer token are not this format.

[connection/header.go](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/connection/header.go)
encodes header pairs as `RawStdBase64(name):RawStdBase64(value)`, separated by
semicolons. The project preserves duplicates/empty values and rejects invalid
HTTP names or CR/LF/NUL in values. Request/response serialized-header support is
advertised explicitly.

Response headers are packed in `cf-cloudflared-response-headers`; Content-Length
is also an ordinary HTTP/2 field. Response metadata identifies origin versus
connector. Generic WebSocket 101/200 mapping remains outside the monitor subset.

## Registration and configuration

The control stream carries Cap'n Proto RPC. The bounded implementation executes
bootstrap, imported capability retention, RegisterConnection, Return/Finish, and
Unregister/Finish/Release on stop. It does not implement general promise pipelines,
exports or third-party capabilities.

The [upstream schema](https://github.com/cloudflare/cloudflared/blob/f11dea9cb7079e90a982c1a2d5548ab40847fdcf/tunnelrpc/proto/tunnelrpc.capnp)
and independent Go oracle validate the C reader/writer. See
[oracle instructions](../tools/rpc_reference/README.md). Features currently
advertised are only `allow_remote_config` and `serialized_headers`.

Configuration arrives on an `update-configuration` HTTP/2 stream as JSON
`{"version":N,"config":{...}}`. The response carries `lastAppliedVersion` and
`err`. The accepted subset is one exact hostname/service, final `http_status:404`,
empty origin options and optional WARP disabled. Invalid updates retain the
last valid configuration. Unknown routing/security options are never silently
discarded.

## HTTP/2 limits

The component uses pinned nghttp2 for framing/HPACK, 2048-byte application chunks,
bounded header scratch and a 65536-byte custom allocator. Control/config have
separate contexts. Local application admission is two/four streams independently
of cloudflared's large advertised wire capacity. See
[design decisions](decisions.md) for the live reload compatibility change.

The library has one task and no per-request socket/task. Native service labels
are not loopback proxy destinations. Full protocol conformance, long hardware
soak and broader origin interoperability are not claimed by the current tests.
