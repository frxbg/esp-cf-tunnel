# Roadmap

## Available in 0.3.2

- Standalone ESP-IDF component with pinned upstream references and licenses.
- Bounded token/header/JSON/Cap'n Proto codecs and an independent RPC oracle.
- IPv4 edge discovery, verified TLS, HTTP/2, registration and remote configuration.
- Native response streaming, fixed admission limits, retry and retained diagnostics.
- S3 System Monitor with AP provisioning and authenticated local/remote API.
- Host/sanitizer tests and S3/P4 compile profiles with and without PSRAM.

## Next work

1. Measure long-duration, multi-client and repeated reconnect/stop-start behavior.
2. Add deterministic TLS/mock-edge failure tests and hardware streaming measurements.
3. Implement deferred inbound backpressure and broaden HTTP interoperability.
4. Evaluate WebSockets, SSE and an explicit optional local HTTP backend.
5. Design multi-edge failover against a measured RAM/CPU budget.

No WARP, QUIC, arbitrary LAN proxy or external gateway is part of the current
scope. Completion evidence and untested cases are tracked in
[validation](validation.md), [compatibility](compatibility.md) and
[memory budget](memory-budget.md).
