# Independent RPC oracle

Host-only Go code; no Go runtime or generated Go schema is linked into firmware.
Fixtures and registration requests contain fixed synthetic credentials only.
The server uses cloudflared's unchanged generated Cap'n Proto schema and the
same pinned zombiezen RPC runtime. The C peer executes bootstrap, registration,
result/finish, unregister and capability release over pipes fragmented one byte
at a time. Unsupported/negative wire fixtures are checked separately in C.

With Go 1.26 or newer in PATH, prepare the pinned reference from a fresh clone:

```sh
git clone --depth 1 --branch 2026.9.1 https://github.com/cloudflare/cloudflared.git .cache/cloudflared
git -C .cache/cloudflared rev-parse HEAD
```

The commit must match `cloudflared-reference.commit` in `dependencies.lock.json`.
Build the C host tests first, then on Windows:

```powershell
python tools/check_rpc_reference.py --client build-host/Debug/cf_rpc_tests.exe
```

On Linux use `--client build-host/cf_rpc_tests`. Use `--go /path/to/go` if Go is
not in PATH.
The runner checks the reference commit and generated schema for modifications,
builds the oracle in a temporary directory, regenerates all fixture bytes for
comparison, decodes the C writer's output and runs the actual RPC server/client.
It never overwrites checked-in fixtures or accesses device credentials.

To intentionally regenerate synthetic fixtures after a reviewed schema change:
build this module and run `rpc-reference generate tests/fixtures/rpc` from the
repository root. Commit the pin, oracle and reviewed fixture changes together.
