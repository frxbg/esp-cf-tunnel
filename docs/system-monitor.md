# System Monitor

The ESP-IDF v6.1 example targets the YD-ESP32-23 ESP32-S3 with 16 MiB flash and
optional 8 MiB octal PSRAM. Firmware and UI are English. It demonstrates native
tunnel serving and shared local/remote administrative handlers.

## Build

Activate ESP-IDF v6.1, then run from the repository root:

```sh
python tools/prepare_idf_tls.py --output .cache/idf-v6.1-tls/esp-tls
python tools/build_monitor.py --profile yd_s3 --tls-override .cache/idf-v6.1-tls/esp-tls
```

`--profile no_psram` disables external RAM. This example is S3-specific; the
network-free protocol smoke example also compiles for P4. Build scripts never flash.
An existing sdkconfig with rollback disabled must enable
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; the monitor rejects a build without it.

## First installation

Replace `PORT` with your serial port and `A1B2C3` with the last six hexadecimal
digits of your device's station MAC. These are documentation placeholders.

```sh
python -m esptool --chip esp32s3 --port PORT read-mac
python -c "from pathlib import Path; Path('.cache/device-backups').mkdir(parents=True, exist_ok=True)"
python -m esptool --chip esp32s3 --port PORT read-flash 0 0x1000000 .cache/device-backups/before-monitor.bin
python tools/provision_monitor.py --device A1B2C3
python tools/flash_monitor.py --port PORT --device A1B2C3 --first-install --backup .cache/device-backups/before-monitor.bin
```

Use a new backup filename if one already exists. First install replaces the
partition table and provisions fresh NVS; it is not an ordinary update.
`flash_monitor.py` checks the MAC and flash configuration before writing.

Provisioning generates a unique random password in
`secrets/A1B2C3/device-access.json` and private NVS images. It does not print the
password or put it in firmware source. The directory and flash backups are
ignored by Git and must remain private. No such device files are distributed.

## Configure the device

1. Join the WPA2 network `ESP-Monitor-A1B2C3` using the locally generated password.
2. Open **http://192.168.4.1**. This AP has no captive DNS portal or Internet access.
3. Open **Settings** and unlock with the same device password.
4. Press **Scan**, choose a network from the visible list, enter its password and
   press **Save and connect**. Selection alone does not change the connection.
5. Read the assigned station IP from Connection or the 115200-baud serial log.
6. Optionally save the named tunnel's connector-install token and public hostname.

For the hostname in Cloudflare, select **HTTP / localhost:80**, followed by
`http_status:404`, with no additional origin options. This is the native backend
label, not a TCP loopback proxy. Configure DNS and Cloudflare Access separately.

An account API key is not a tunnel token. Blank token preserves the stored token;
**Clear stored token** removes it. A blank Wi-Fi password preserves the old one
only for the same SSID; open networks require the explicit checkbox.

The setup AP remains available until Wi-Fi connects, then for ten minutes. Hold
BOOT for three seconds to reopen it. A lost station connection reopens setup and
retries Wi-Fi. An AP channel change during association may require rejoining.

## Updates and OTA

The development monitor version is **0.4.0-dev**. First migrate an existing
0.3.x installation over USB, retaining both NVS partitions:

```sh
python tools/flash_monitor.py --port PORT --device A1B2C3 --boot-factory
```

This explicitly initializes OTA boot selection and installs the bootloader,
partition table and factory application. Do not use `--first-install` for an
existing configured device. No full-flash erase, NVS write or eFuse change occurs.
Subsequent ordinary USB writes omit OTA metadata; use `--boot-factory` when you
intend to boot the newly written factory image (also useful for USB recovery).

In **Settings**, unlock with the device password, choose
`build-monitor-yd_s3/esp_system_monitor.bin`, then **Upload and restart**.
Use only a trusted application image built for this board, not a bootloader,
partition table or merged flash image. The same UI/API works over LAN, setup AP
and the configured Cloudflare hostname; Cloudflare Access remains a separate gate.
Keep the page open and power connected. Uploads can be cancelled before final
verification; interrupted uploads expire after 60 seconds without a chunk.
Restart a failed upload from the beginning. A device session expires after ten
minutes, so unlock immediately before starting an update over a slow connection.

| Partition | Address | Size |
|---|---|---|
| Factory application | `0x10000` | 1.5 MiB |
| System NVS (unchanged) | `0x200000` | 24 KiB |
| Monitor NVS (unchanged) | `0x206000` | 24 KiB |
| PHY data (unchanged) | `0x20c000` | 4 KiB |
| OTA boot metadata | `0x20d000` | 8 KiB |
| OTA slot 0 | `0x220000` | 1.5 MiB |
| OTA slot 1 | `0x3a0000` | 1.5 MiB |

Uploads write only the inactive OTA application slot. After SHA256 and ESP image
validation, the device selects it for the next boot and restarts. The bootloader
has rollback enabled; the application confirms boot after storage, network,
HTTP server and tunnel-task initialization. This checks startup, not long-term
Cloudflare reachability. A network outage alone does not roll back valid firmware.
SHA256 detects corruption; it is **not** a publisher signature. Secure Boot,
anti-rollback eFuses and signed firmware distribution are not configured here.
The behavior follows [ESP-IDF OTA/rollback](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ota.html).

### Bounded upload API

All POSTs require the existing device bearer session and allowed Origin/Host:

- `/api/ota/start`: `{size, sha256}` returns a random upload `id` and `chunk_size`.
- `/api/ota/chunk`: `{id, offset, data}`; data is base64 of at most 1408 bytes.
  Offset must equal bytes already accepted; duplicate/out-of-order chunks fail.
- `/api/ota/finish`: `{id}` verifies the entire upload and schedules restart.
- `/api/ota/abort`: `{id}` discards the inactive upload without rebooting.

Requests remain below the existing 2048-byte JSON limit. The firmware uses
incremental flash erase and PSA SHA256; it never buffers the whole application
or creates an OTA task. Invalid chip/project/header data is rejected before
opening flash. `/api/status` reports progress, capacity and slots but no upload id.
`tools/test_monitor_ota.py` runs hardware rejection checks; optional `--upload`
installs the supplied app, and `--cycles` exercises tunnel stop/start. Its hidden
password prompt avoids credentials in command lines, files and reports.

## Pages and controls

| Page | Features |
|---|---|
| Overview | Uptime, internal RAM/minimum/largest block, RAM graph, chip temperature, RSSI, flash/PSRAM and CPU frequency |
| Inputs / Outputs | GPIO 4-7 mode/level and separate GPIO48 RGB color/brightness controls |
| Connection | Network, clock, registration/config, retry count, TCP/TLS error details, SNTP confirmations, tunnel restart and task headroom |
| Settings | Device-password unlock, Wi-Fi scan/selection, network/tunnel configuration and OTA upload |

GPIO 4-7 start disabled; verify wiring before applying an output. GPIO48 starts
off. On the [YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23), RGB control
requires its RGB solder jumper to be connected. Inspect with power disconnected;
do not bridge the unrelated IN-OUT or USB-OTG pads. USB, UART, BOOT, flash/PSRAM
and RGB pins are excluded from the general GPIO controls.

## Authentication and diagnostics

Local and tunneled administration use the same device password and ten-minute
bearer session. A new login replaces the previous session. Cloudflare Access
login does not replace device authentication. GET status never exposes secrets.
There are no unauthenticated write endpoints or CORS grants. See [security](security.md).

The scan list shows up to twelve AP records with SSID, RSSI, security and channel;
hidden names can be typed manually. Selecting another SSID clears the password
field and survives periodic status refreshes until saved.

Connection failures and the last failure persist across reconnects until restart.
The status API also exposes the failure's uptime timestamp and current rejected
stream count. Increased free RAM during reconnect reflects the released TLS/H2
objects; inspect the retained diagnostic for the trigger. The connector uses four
app slots and one edge connection. Online means registration and ingress config
succeeded; it is separate from Cloudflare's high-availability dashboard status.

During a browser reload, the local HTTP server may log `error in send : 104`
followed by `uri handler execution failed`. Error 104 is `ECONNRESET`: the peer
reset a connection while the server was sending. A reload can cancel a pending
asset or telemetry request; the handler then returns the send error and HTTPD
closes that connection. Occasional warnings with a successful reload do not
establish a device crash or RAM shortage. Investigate repeated warnings without
reloads, incomplete pages or stalled requests separately.
