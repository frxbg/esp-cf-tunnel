# Design decisions

| Decision | Reason |
|---|---|
| Native C / ESP-IDF 6.1 | Direct embedded integration; no Go runtime or external gateway |
| Separate component and examples | Applications own networking, clock, storage and authorization |
| nghttp2 server over outgoing TLS client | Matches cloudflared's reversed HTTP/2 connection roles |
| Bounded custom allocator and admission | Predictable memory ceilings and explicit overload handling |
| Private cJSON symbols and lock contract | Avoid symbol collisions and serialize parser state |
| One transport owner task | No task/socket per request; clear buffer lifetime |
| Strict remote-config subset | Unsupported security/routing options fail explicitly |
| Independent Go RPC oracle | Validate C framing/capability lifecycle against upstream schema/runtime |
| Equal-jitter backoff | Bound retry frequency and honor server minimum retry delay |
| Explicit stop/deinit | Application retains context until asynchronous shutdown completes |
| PSRAM optional | No blanket migration of internal/TLS allocations into external RAM |

## HTTP/2 capacity

The wire setting matches cloudflared's `MAX_CONCURRENT_STREAMS=UINT32_MAX`.
Local application admission remains two/four slots, with separate control and
configuration storage and a hard nghttp2 quota. Low wire limits produced live
GOAWAY(NO_ERROR) during browser reloads; capacity retirement by the edge pool is
inferred from the before/after results, not its private implementation.

The monitor enables four existing app slots. Overload tests verify refusal of
extra requests while control/config remain available. We did not weaken upstream
GOAWAY validation or imitate an official cloudflared release number. The one-edge
mode is independent of the dashboard's Healthy/Degraded interpretation.

## Diagnostics and credentials

Retain one bounded local failure message with timestamp/count and numeric
TLS/HTTP2/peer metadata across reconnects. Incoming raw debug data is not kept.
Locally generated nghttp2 GOAWAY reasons are fixed strings in the pinned source.
Tokens and buffers containing them are wiped and never included in diagnostics.

Dependency revisions are in [the lock file](../dependencies.lock.json), protocol
sources in [protocol.md](protocol.md), and measured limits in
[memory-budget.md](memory-budget.md).
