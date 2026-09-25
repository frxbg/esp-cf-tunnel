# TCP/TLS connect investigation

## Baseline recorded before changes (2026-09-25)

- Project: `eeb491268e30b0e692785d203e29100830223ea0`; clean working tree,
  identical to the requested reference revision.
- SDK: ESP-IDF `v6.1`, commit `fff9895c82d744c7237be8847347bdd1b07c6643`.
  No tracked SDK modifications. An unrelated generated requirements file exists.
- lwIP submodule: `c6f2f878e7b0f86033214b85547d579be43351e3`.
- Target/profile: ESP32-S3 / `yd_s3`, 16 MiB flash, octal PSRAM enabled with
  capability-based allocation. The monitor application version is 0.3.2.
- Effective `build-monitor-yd_s3/sdkconfig` SHA256:
  `2d255e1259db8947ea13b6891daf36249f9291262d7de8d753569f16f7d2cbdc`.
  A full local copy is retained under ignored `.cache/tls-fix/`.
- Effective transport settings: mbedTLS, TLS 1.2, verified certificates/SNI,
  insecure mode disabled, incoming/outgoing record buffers 16384/4096 bytes,
  four application streams, 2048-byte chunks, ten lwIP sockets, 100 Hz RTOS tick.
- Baseline `esp_tls.c` SHA256:
  `ee7ddb63bb9328bb9aa44f8889081c0e5316f593c2466cb6da441f30b8989c48`.
- Baseline lwIP `src/api/sockets.c` SHA256:
  `861822d05ac60f68228279036a966c26acb1282b8f2f7bd0c4672129c7fd74a4`.

## Sequence

1. Add retained structured diagnostics before destroying the TLS handle; retain
   the existing 1 ms polling interval and 30 s application deadline.
2. Reproduce and test the SDK asynchronous-connect defect against the exact
   source. Package a small version/hash-guarded patch; never modify the global
   SDK implicitly.
3. Build and test using an explicit project-local SDK component override.
4. Record hardware evidence separately from host tests. A source defect alone
   does not prove the cause of an individual device failure.

## Runtime diagnosis and repair (2026-09-25)

There were two distinct failure patterns, not a token/CA diagnosis:

1. **Proven TLS protocol rejection:** the ESP reported ESP `0x801a`, SDK TLS
   magnitude `30592` (`0x7780`, mbedTLS fatal alert), peer alert **70**
   (`protocol_version`), state `HANDSHAKE -> FAIL`, in 51 ms. TLS 1.2 was the
   effective build setting. A separate PC check against an edge from the same
   network rejected TLS 1.2 and verified TLS 1.3; that PC result alone was not
   treated as device evidence. Enabling TLS 1.3 then produced a verified TLS 1.3
   connection **on the ESP**, followed by H2, registration, configuration and Online.
2. **Confirmed SDK poll defect, with a matching device symptom:** source inspection
   and execution of the exact SDK function reproduced fd_sets becoming empty
   after a first timeout, ignored negative select results, and ignored nonzero
   SO_ERROR. Separately, the unpatched ESP recorded a 30029 ms application
   deadline while still CONNECTING, rc=0, with no ESP/TLS/system/verify error.
   That trace is consistent with the reproduced defect, but does not itself
   inspect socket readiness or prove that every real deadline had that cause.

The [version/hash-guarded patch](../patches/esp-idf-v6.1/README.md) restores sets
on every poll and handles select/SO_ERROR. The SDK directory is unchanged;
builds explicitly use a checked project-local ESP-TLS component copy.
No timeout workaround, transport replacement or new tunnel task was introduced.

### Selected device traces

These fields were retained before handle destruction. Public edge addresses are
shown; no device/network names, tokens, sessions or raw private logs are included.

| Field | Failed TCP progress | Failed TLS protocol | Verified TLS 1.3 |
|---|---|---|---|
| Stage | application_deadline | tls_handshake | verified |
| rc | 0 | -1 | 1 |
| State before/after | 1 / 1 | 2 / 3 | 2 / 4 |
| ESP / TLS / verify / system | 0 / 0 / 0 / 0 | 32794 / 30592 / 0 / 0 | 0 / 0 / 0 / 0 |
| Peer alert | unavailable | 70 | unavailable |
| Negotiated version | unavailable | unavailable | 772 (`0x0304`) |
| errno context | 119 | 5 | 11 |
| Elapsed ms | 30029 | 51 | 1008 |
| Edge / port | 198.41.200.33:7844 | 198.41.200.13:7844 | 198.41.192.47:7844 |
| UTC (Unix seconds) | 1790361395 | 1790361445 | 1790361843 |
| Free / largest internal bytes | 205928 / 155648 | 175764 / 131072 | 168184 / 126976 |

The diagnostic prototype returned a negative unavailable-alert sentinel; the
final code normalizes it to -1. Errors are copied once, SYSTEM before the
clearing last-error accessor. Positive TLS error magnitudes are retained as
reported by ESP-IDF rather than rewritten. The successful trace's errno=11
illustrates why raw errno is context, not the cause or a success criterion.
An rc=1 also wins over a simultaneously reached application deadline.

SNI remains `h2.cftunnel.com`, CA PEM length still includes the embedded NUL,
and certificate/hostname verification stays enabled. No ALPN was added and no
token, DNS or ingress settings were changed. The reference
[cloudflared HTTP/2 protocol](https://github.com/cloudflare/cloudflared/blob/2026.9.1/connection/protocol.go)
uses that SNI; QUIC-specific ALPN is not copied into this transport.

`clock_valid` remains the existing year-after-2024 readiness guard, not proof of
accurate time. New SNTP callback count/last-sync UTC distinguish a real sync;
the verified run reported one sync. Monotonic timers drive deadlines. No
certificate verification or allocation failure was observed in these traces,
so no speculative CA replacement, insecure setting or buffer/stack expansion
was applied.

## Tests and hardware scope

- Windows MSVC host CTest: **11/11** suites, including public-API diagnostic
  classification, retained numeric errors and retrieval order.
- Actual SDK function harness: **45** baseline assertions (old defects reproduced)
  and **876** patched assertions. First timeout then readiness, EINTR/EAGAIN,
  refused/unreachable, getsockopt failures, pending/error paths, zero timeout
  semantics and 100 cooperative stop/restart sequences pass.
- ASan/UBSan for the new SDK harness: **NOT RUN locally**; the available WSL
  environment lacks CMake. The checked-in Linux CI command enables both. Consult
  the [CI runs](https://github.com/frxbg/esp-cf-tunnel/actions/workflows/ci.yml)
  for results on a published commit; historical 0.3.2 CI is a separate result.
- ESP32-S3 YD and no-PSRAM profiles build with the explicit override. The YD
  profile runs on hardware with TLS 1.3 and
  bootloader rollback. No tracked SDK changes and no NVS erase/write were made.
- **12 real tunnel stop/deinit/init/start cycles** recovered Online. Each cycle
  served 13-14 status polls during reconnection; none failed and uptime increased.
  Stable-state free internal RAM ranged **128088-128204 bytes** (first 128108,
  last 128204), largest block **55296-86016**, minimum tunnel stack headroom
  **6572 bytes**, and task count stayed **11**. This short run shows no declining
  free-heap trend, not a long-duration fragmentation guarantee.
- Serial capture showed no watchdog or panic. Its two HTTP gaps coincided with
  the intentional OTA restart; the reconnect-only status checks had zero gaps.
- OTA hardware/API: 21 check groups including auth/origin rejection, wrong
  magic/chip/project, malformed digest/base64/offsets, duplicates, incomplete
  upload, cancel, SHA mismatch, truncated ESP image and successful factory-to-
  ota_0 boot. Wi-Fi, tunnel configuration and the original device password were
  retained. Details and reproduction are in [System Monitor](system-monitor.md).
- JavaScript SHA256: Node crypto comparison for empty/abc, block/padding
  boundaries and a full 1.5 MiB slot passed.

### Browser checks

Playwright 1.58.2 / headless Edge was used because the Browser plugin was not
available. On the real LAN device, Settings unlock and file selection survived
periodic refreshes; Cancel stopped a real upload and re-enabled the form.
The following full browser upload reached visible progress, but its completion
could not be verified: LAN stopped responding and Windows enumerated **no serial
ports**, including the previously connected device. This does not identify the
cause of the disappearance. The earlier API-driven OTA boot above is a separate,
completed hardware result. A reconnect is needed to finish the browser reboot test.

UI-only fallback used an explicitly synthetic status fixture (no live backend)
for desktop 1365x960 and mobile 393x852. Page identity, meaningful content,
no error overlay, zero JavaScript/console errors, unlock, diagnostic expansion
and no horizontal overflow passed; screenshots were inspected outside Git.
Do not interpret this fixture as proof of live upload completion or recovery.

Final source publication guard and Gitleaks 8.30.1 scan of the publishable
working-tree inventory passed; no credentials or private captures were included.
Both monitor build profiles pass the explicit rollback-enabled configuration
check. The checked-in binary inventory reports still describe the older release.

Still **NOT RUN**: deliberate power-cut/failed-boot rollback, live OTA through
Cloudflare Access, 100 real start/stop cycles, long soak, P4/no-PSRAM hardware,
and controlled physical TCP fault injection. Host cancellation is not claimed
as a hardware scheduler test. Future failures should preserve `tunnel_connect`,
`tunnel_connect_failure`, SNTP fields and heap/largest values from the API.

These features are unreleased development changes. Private captures remain
ignored and are not publication artifacts.
