# Vendored libnghttp2

Upstream: https://github.com/nghttp2/nghttp2, v1.70.0,
commit `85e300c79fb6dbcfa9c1013215c8710c1c2cd3d2`. License: [COPYING](COPYING).

Only the library sources and public headers are included. `library.cmake`
selects the upstream library source list and target-local platform defines;
`nghttp2ver.h` is generated with this pinned version. No upstream executables,
TLS implementation, compression dependency, install rules or SDK patches.

One portability patch in `lib/nghttp2_map.c`: the debug printer uses `PRId32`
instead of `%d` for int32_t stream IDs. ESP's int32_t is long, so the original
debug function fails ESP-IDF's format checks even when unused. Protocol behavior
is unchanged. The library is exercised with the C HTTP/2 client/server host
harness and GCC ASan/UBSan; see `tools/test_h2.sh`.
