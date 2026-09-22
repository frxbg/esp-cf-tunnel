# Protocol smoke example

Compiles and links the portable protocol core and ESP transport for ESP32-S3/P4.
It exercises codec checks and connector init/start/stop/deinit with networking
intentionally unavailable. It contains no credentials and does not create a
live tunnel.

From the repository root in ESP-IDF v6.1:

```sh
sh tools/build_idf.sh esp32s3 no_psram
sh tools/build_idf.sh esp32p4 psram
```

PowerShell: `tools/build_idf.ps1 -Target esp32s3 -Profile no_psram`.
Both targets have PSRAM/no-PSRAM profiles. Verify board/revision and RAM hardware
before flashing; compilation is not proof that a profile matches your board.

For a complete working application, use [System Monitor](../system_monitor).
