# Contributing

Use native C and keep the component independent of the monitor, board networking
and credential storage. Preserve bounded allocations, secret wiping, explicit
unsupported-feature errors and the single-owner transport design.

## Local checks

```sh
python tools/generate_fixtures.py --check
cmake -S . -B build-host
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure
node tests/host/test_sha256.js
python tools/check_publication.py
```

Linux/WSL sanitizer checks: `sh tools/test_host.sh`, `sh tools/test_h2.sh`, and
`sh tools/test_monitor_remote.sh`. Clang with `-DCF_FUZZING=ON` additionally
provides `cf_parser_fuzz`; run it against a temporary corpus with a time limit.

After activating ESP-IDF v6.1, compile both targets and memory profiles:

```sh
python tools/prepare_idf_tls.py --output .cache/idf-v6.1-tls/esp-tls
python tools/test_idf_tls.py
sh tools/build_idf.sh esp32s3 no_psram .cache/idf-v6.1-tls/esp-tls
sh tools/build_idf.sh esp32s3 psram .cache/idf-v6.1-tls/esp-tls
sh tools/build_idf.sh esp32p4 no_psram .cache/idf-v6.1-tls/esp-tls
sh tools/build_idf.sh esp32p4 psram .cache/idf-v6.1-tls/esp-tls
python tools/build_monitor.py --profile yd_s3 --tls-override .cache/idf-v6.1-tls/esp-tls
python tools/build_monitor.py --profile no_psram --tls-override .cache/idf-v6.1-tls/esp-tls
```

PowerShell users can use `tools/build_idf.ps1 -Target esp32s3 -Profile no_psram`
with `-TlsOverride .cache/idf-v6.1-tls/esp-tls` and the corresponding target/profile
combinations. Build commands never flash. The [patch guide](patches/esp-idf-v6.1/README.md)
explains exact revision/hash guards and checks; the global SDK is never modified.

The [independent RPC oracle](tools/rpc_reference/README.md) needs Go and the
pinned reference checkout; ordinary C tests need neither. Update dependency
pins, notices and affected fixtures together, with separate evidence for host,
compile-only and real hardware results.

## Pull requests

Explain the behavior change, relevant validation and remaining limits.
Avoid unrelated reformatting of vendored sources. Do not submit device tokens,
passwords, NVS/flash dumps, real network identifiers, private screenshots or
SDK/build output. Synthetic test vectors must be clearly marked.

## Packaging

`python tools/package_component.py` creates the standalone component archive
and checksum under ignored `dist/`. Stage intended changes first: packaging reads
the Git index so archive bytes use canonical line endings on every platform.
It runs the publication guard first; CI also scans history with pinned Gitleaks.
GitHub source archives include examples, documentation and tests; the component
archive contains only the installable library and its licenses.
