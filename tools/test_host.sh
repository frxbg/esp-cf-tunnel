#!/usr/bin/env sh
# Lightweight GCC/Clang sanitizer runner for hosts without CMake.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
mkdir -p build-host-sanitize
component=components/esp_cf_tunnel
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCJSON_NESTING_LIMIT=16 \
    -I"$component/include" -I"$component/src/protocol" \
    -I"$component/third_party/cjson" -Itests/fixtures \
    "$component"/src/protocol/*.c "$component/src/cf_lifecycle.c" \
    "$component/third_party/cjson/cf_cJSON.c" tests/host/test_core.c -lm \
    -o build-host-sanitize/cf_core_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 build-host-sanitize/cf_core_tests
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCJSON_NESTING_LIMIT=16 \
    -I"$component/include" -I"$component/src/protocol" \
    -I"$component/third_party/cjson" -Itests/fixtures/rpc \
    "$component"/src/protocol/*.c "$component/src/cf_lifecycle.c" \
    "$component/third_party/cjson/cf_cJSON.c" tests/host/test_rpc.c -lm \
    -o build-host-sanitize/cf_rpc_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 build-host-sanitize/cf_rpc_tests
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCJSON_NESTING_LIMIT=16 \
    -I"$component/include" -I"$component/src/protocol" \
    -I"$component/third_party/cjson" -Itests/fixtures \
    "$component"/src/protocol/*.c "$component/src/cf_lifecycle.c" \
    "$component/third_party/cjson/cf_cJSON.c" tests/host/fuzz_parsers.c \
    tests/host/mutation_smoke.c -lm -o build-host-sanitize/cf_mutation_smoke
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 build-host-sanitize/cf_mutation_smoke
