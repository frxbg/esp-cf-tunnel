#!/usr/bin/env sh
set -eu
target=${1:-esp32s3}
profile=${2:-no_psram}
case "$target" in esp32s3|esp32p4) ;; *) echo 'Target must be esp32s3 or esp32p4' >&2; exit 2;; esac
case "$profile" in no_psram|psram) ;; *) echo 'Profile must be no_psram or psram' >&2; exit 2;; esac
: "${IDF_PATH:?Activate ESP-IDF v6.1 first}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
example="$root/examples/protocol_smoke"
build="$root/build-idf-$target-$profile"
python "$IDF_PATH/tools/idf.py" -C "$example" -B "$build" \
    -D "IDF_TARGET=$target" -D "SDKCONFIG=$build/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$example/sdkconfig.defaults;$example/profiles/$profile.defaults" build
