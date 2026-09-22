#!/usr/bin/env sh
# HTTP/2 wrapper and vendored upstream library with ASan/UBSan.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
component=components/esp_cf_tunnel
mkdir -p build-host-sanitize
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DNGHTTP2_STATICLIB -DBUILDING_NGHTTP2 -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H \
    -DHAVE_CLOCK_GETTIME -DHAVE_DECL_CLOCK_MONOTONIC=1 -D_POSIX_C_SOURCE=200809L \
    -I"$component/include" -I"$component/third_party/nghttp2/lib/includes" \
    "$component/src/transport/cf_h2.c" "$component/src/protocol/cf_headers.c" \
    "$component/src/protocol/cf_base64.c" "$component/third_party/nghttp2/lib/"*.c \
    tests/host/test_h2.c -o build-host-sanitize/cf_h2_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 build-host-sanitize/cf_h2_tests
