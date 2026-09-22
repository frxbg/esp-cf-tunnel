#!/usr/bin/env sh
# Exercise the exact remote header/origin parser with ASan/UBSan.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
component=components/esp_cf_tunnel
mkdir -p build-host-sanitize
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -DNGHTTP2_STATICLIB \
    -I"$component/include" -I"$component/third_party/nghttp2/lib/includes" \
    -Iexamples/system_monitor/main \
    "$component/src/transport/cf_h2.c" "$component/src/protocol/cf_headers.c" \
    "$component/src/protocol/cf_base64.c" examples/system_monitor/main/monitor_remote.c \
    tests/host/test_monitor_remote.c -o build-host-sanitize/monitor_remote_tests
# Only the real cf_h2_header accessor is reachable from this parser test;
# the linker drops unrelated H2 session code (covered by test_h2.sh).
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 build-host-sanitize/monitor_remote_tests
