# Changelog

## 0.3.2 - 2026-09-22

Initial public ESP-IDF library distribution.

- Package the native connector with metadata, licenses and integration documentation.
- Match cloudflared's advertised HTTP/2 stream capacity while retaining fixed local admission and heap limits.
- Enable four application streams in the System Monitor example.
- Retain the last connection failure and peer/local GOAWAY diagnostics across reconnects.
- Include bounded host tests, sanitizer scripts and the S3/P4 compile matrix.

### Included from development builds

- 0.3.1: visible Wi-Fi scan selection and shared authenticated monitor API over LAN/AP and the tunnel.
- 0.3.0: live named tunnel registration/configuration and native monitor HTTP serving.
- Earlier development: portable codecs, RPC oracle, AP provisioning and system/GPIO/RGB telemetry.

Versions before 0.3.2 were local development builds, not public releases.
