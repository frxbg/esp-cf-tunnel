# Changelog

## Unreleased

- Retain structured TCP/TLS diagnostics, including system errors, peer alerts,
  elapsed time and heap measurements, before destroying the connection handle.
- Add a guarded, explicit ESP-IDF v6.1 SDK poll patch and regression harness.
- Enable verified TLS 1.3 for the native connector after measured edge rejection
  of TLS 1.2; preserve SNI, CA validation and bounded 1 ms polling.
- Add authenticated, bounded OTA uploads to the English System Monitor UI
  (0.4.0-dev), with inactive slots, integrity/image checks and startup rollback.
- Show actual SNTP confirmations and offer authenticated tunnel restart.

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
