# System Monitor

The ESP-IDF v6.1 example targets the YD-ESP32-23 ESP32-S3 with 16 MiB flash and
optional 8 MiB octal PSRAM. Firmware and UI are English. It demonstrates native
tunnel serving and shared local/remote administrative handlers.

## Build

Activate ESP-IDF v6.1, then run from the repository root:

```sh
python tools/build_monitor.py --profile yd_s3
```

`--profile no_psram` disables external RAM. This example is S3-specific; the
network-free protocol smoke example also compiles for P4. Build scripts never flash.

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

## Updates

Rebuild and omit `--first-install` to retain Wi-Fi, tunnel and device credentials:

```sh
python tools/flash_monitor.py --port PORT --device A1B2C3
```

The script writes only build images for an ordinary update, checks the board MAC
and verifies the write through esptool. It never changes eFuses. The layout is:
app at `0x10000` (1.5 MiB), system NVS at `0x200000` (24 KiB), monitor NVS at
`0x206000` (24 KiB). Verify board/flash capacity before first install.

## Pages and controls

| Page | Features |
|---|---|
| Overview | Uptime, internal RAM/minimum/largest block, RAM graph, chip temperature, RSSI, flash/PSRAM and CPU frequency |
| Inputs / Outputs | GPIO 4-7 mode/level and separate GPIO48 RGB color/brightness controls |
| Connection | Network, clock, registration/config, retry count, last failure, HTTP/2 memory and task stack headroom |
| Settings | Device-password unlock, Wi-Fi scan/selection and network/tunnel configuration |

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
