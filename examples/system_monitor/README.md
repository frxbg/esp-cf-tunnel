# System Monitor example

Native ESP-IDF application for the YD-ESP32-23 ESP32-S3: English telemetry UI,
password-protected setup AP, Wi-Fi scan/selection, network/tunnel settings,
GPIO 4-7 and built-in GPIO48 RGB control. Four application streams share the
native tunnel; local HTTPD and remote requests use the same authenticated API.

From the repository root, in an activated ESP-IDF v6.1 environment:

```sh
python tools/build_monitor.py --profile yd_s3
```

Use `--profile no_psram` for the compile profile without external RAM. The example
expects **16 MiB flash**; verify board configuration before flashing. RGB control
requires the board's RGB solder jumper to connect the LED data input.

Follow [setup, provisioning and flash instructions](../../docs/system-monitor.md).
There is no default shared password or supplied NVS image. Provision each device
locally, then enter Wi-Fi credentials and the tunnel token in Settings.

Read [security](../../SECURITY.md) before exposing administrative controls.
