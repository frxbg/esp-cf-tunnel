# Memory and measurements — 0.3.0

The table and baseline measurements below describe 0.3.0. Newer measurements
are in the versioned follow-ups below. Measurements are
observations of this build, not a complete allocation budget or a no-PSRAM
runtime guarantee. The active YD profile enables 8 MiB PSRAM; normal allocations
have not been globally redirected there.

| Resource | Bound / ownership |
|---|---|
| Token input / decoded scratch | 1024 / 769 bytes; caller, wiped |
| Secret bytes | 32–128, runtime copy wiped after registration encoding |
| Header codec | 6144-byte wire, 4096 decoded bytes, 32 fields |
| H2 incoming headers | 8192 bytes / 48 fields per session scratch |
| H2 engine | 16184 bytes on host; target sizeof may differ |
| nghttp2 allocations | 65536-byte ceiling including our allocation prefix and realloc transient peak; system heap metadata excluded |
| H2 slots | 2 or 4 application slots + control + config; monitor uses 2 |
| Application chunk | 2048 bytes, not a TLS record/frame limit |
| HPACK / stream receive window | 1024 / 2048 bytes; initial protocol window behavior still applies |
| RPC message | 16384 bytes, 8 segments, 4096 traversal words, depth 32 |
| RPC TX queue | 4096 bytes plus bounded 1024-byte temporary encoder buffer |
| Config message | 8192 bytes; owner, wiped after parsing |
| Private JSON heap / nesting | 32768 bytes / depth 16; shared app mutex |
| Tunnel task stack | 12288 bytes |
| TLS receive | 16384-byte record buffer, no insecure size reduction |
| Native status response | At most 6144 allocated bytes per active status request; flash assets streamed directly |

The H2 constructor's small temporary option/callback objects use upstream's
default allocator and are freed before return. The runtime context, task,
DNS answer, TLS allocations, lwIP and app state are outside the nghttp2 quota.
The connector uses one owner task and one edge socket; a temporary UDP DNS socket
is closed before TLS. Body bytes are not accumulated into a full upload/download.

## Observed results

- Host 10 MiB upload/download: nghttp2 peak 24537 bytes on MSVC, 24793 under GCC
  ASan; checked every byte. These exclude TLS and runtime context costs.
- S3 after initial successful configuration: internal free 141548 bytes,
  lifetime minimum 137236. With browser/auth activity, the observed lifetime
  minimum reached about 69.8 KiB; it includes the whole monitor and transient
  work, not only tunnel allocations. Free RAM returned to about 133 KiB.
- A rendered connected snapshot showed H2 current 22.3 KiB, peak 25.2 KiB,
  one active control stream, tunnel stack headroom 6.2 KiB, 11 tasks and
  8 MiB PSRAM still available. No complete CPU/latency or fragmentation trend yet.
- Monitor firmware: YD-S3 1042336 bytes, no-PSRAM 1032368; only YD profile run
  on hardware. SHA256 and compile matrix are in validation/build-report.

Still required: matched network/UI baseline without the connector; controlled
init/handshake/reconnect/load peaks; precise internal versus PSRAM allocation
accounting; 10 MiB and parallel hardware traffic; stalled consumers; 100 actual
transport stop-start/reconnect cycles; 24-hour soak. The 100 H2 host cycles are
not 100 ESP task/socket/TLS lifecycle tests. Flash bytes are not RAM usage.

## Monitor 0.3.1 changes

The native adapter has one bounded request metadata record per application slot
(path/auth and method/framing flags). JSON POST input allocates up to 2048 bytes;
API replies allocate a 6144-byte payload plus small status/effect metadata.
Request memory is freed/wiped after dispatch; response memory after stream
completion/reset. Optional serialized-header decoding uses a temporary bounded
4096-byte arena and 32 header spans. Assets remain in flash without a body copy.
JSON/auth state is shared with HTTPD under the same mutex. No extra task, socket
or loopback proxy was added. Synchronous NVS commit can briefly pause API work;
an asynchronous save worker and extended load measurements remain future work.


## Monitor 0.3.2 reload regression

The monitor now enables all four existing application contexts. Each active
JSON reply can own 6144 payload bytes plus metadata; POST input is bounded at
2048 bytes per slot. Four replies can therefore overlap instead of two. Flash
assets still have no allocated response body. The wire MAX_CONCURRENT_STREAMS
setting matches cloudflared and is not an allocation allowance: fixed admission
slots and the 65536-byte nghttp2 quota still apply.

The host H2 object is now 16264 bytes, including numeric peer/local GOAWAY and
bounded local-reason diagnostics. GCC ASan/UBSan regression peak was 24793 bytes
of nghttp2 heap. Runtime snapshots retain one 128-byte failure message with time
and count; no history buffer, extra task or extra connection was added.

After user-confirmed public reloads and LAN browser load, at uptime 210436 ms:
internal free 129884 bytes, lifetime minimum 72072, largest block 71680;
nghttp2 current/peak 22928/26084, task stack headroom 6652, one connection
attempt and zero failures/refusals. This short mixed workload is not a matched
baseline. Final HTML-diagnostic build LAN QA: internal free 133048, minimum
79540 bytes, one attempt and zero failures. No-PSRAM firmware was compiled,
not flashed; prolonged four-client contention remains unmeasured.
