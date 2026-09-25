# ESP-IDF v6.1 asynchronous TCP poll fix

Patch v1 applies only to IDF `fff9895c82d744c7237be8847347bdd1b07c6643`
and lwIP `c6f2f878e7b0f86033214b85547d579be43351e3`. The manifest pins the
original source, patch and patched source SHA256 (normalized LF). The helper
also rejects tracked changes to ESP-TLS or lwIP sockets.c.

`select()` overwrites its descriptor sets, including clearing them on timeout.
This patch rebuilds both sets for every poll, handles negative select results,
and checks the **value** of SO_ERROR after getsockopt succeeds. EINTR/EAGAIN and
in-progress connections remain pending. Errors retain their system error code.
The transport keeps `timeout_ms=1`; zero still means a NULL select timeout in
this SDK and must not be used as a nonblocking workaround.

## Apply and check without modifying the SDK

Activate the pinned SDK, then run from the repository root:

```sh
python tools/prepare_idf_tls.py --output .cache/idf-v6.1-tls/esp-tls
python tools/prepare_idf_tls.py --output .cache/idf-v6.1-tls/esp-tls --check
python tools/test_idf_tls.py
python tools/build_monitor.py --profile yd_s3 --tls-override .cache/idf-v6.1-tls/esp-tls
```

The helper applies the actual patch in a temporary directory and copies the
tracked ESP-TLS component into the explicit project-local output. It rejects
an output inside the SDK and never overwrites a different existing override.
`--check` validates every copied file. There is no automatic SDK update.

The example CMake projects accept `-D CF_IDF_TLS_OVERRIDE=/absolute/path/esp-tls`.
Protocol smoke wrappers accept a third shell argument or PowerShell
`-TlsOverride`. Without the explicit option, builds use the original SDK. Other
applications can append the verified override to `EXTRA_COMPONENT_DIRS` before
including IDF's project.cmake. Check the build's component paths.

Do not apply this patch to another SDK revision by bypassing the guards. Recheck
the upstream implementation and regenerate a separately versioned patch/test.
The component archive alone does not contain this SDK override; retain this
patch directory and both preparation/regression tools in your integration.

## Regression coverage

The test extracts the **actual** low-level function from original and patched
SDK source, stubbing only its OS/TLS dependencies. It reproduces the old lost
fd_set, ignored select error and ignored SO_ERROR. Patched tests cover first
timeout then readiness, EINTR/EAGAIN, EBADF, refused/unreachable connections,
getsockopt failures, pending sockets, and cooperative stop/restart polling.
GCC/Clang builds enable ASan/UBSan; MSVC runs the same deterministic assertions.
This harness does not claim to exercise a real FreeRTOS scheduler or network.

See [device evidence and separate TLS protocol issue](../../docs/tcp-tls-diagnostics.md).
