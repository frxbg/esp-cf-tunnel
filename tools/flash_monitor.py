"""Flash a built ESP32-S3 monitor; preserve NVS except on explicit first install."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--device', required=True, help='Expected last 6 hex digits of station MAC')
    p.add_argument('--profile', choices=['yd_s3', 'no_psram'], default='yd_s3')
    p.add_argument('--first-install', action='store_true', help='Write fresh dedicated NVS partitions; requires a full backup')
    p.add_argument('--backup', type=Path)
    p.add_argument('--boot-factory', action='store_true', help='Initialize OTA boot selection to factory; preserves Wi-Fi/device NVS')
    a = p.parse_args()
    if not re.fullmatch('[0-9A-Fa-f]{6}', a.device):
        p.error('Expected 6 hex digits for --device')
    build = ROOT / f'build-monitor-{a.profile}'
    config = json.loads((build / 'flasher_args.json').read_text())
    if config['extra_esptool_args']['chip'] != 'esp32s3' or config['flash_settings']['flash_size'] != '16MB':
        raise SystemExit('Unexpected target/flash configuration.')
    files = {int(offset, 0): build / name for offset, name in config['flash_files'].items()}
    # IDF adds ota_data_initial.bin for an OTA layout. Ordinary updates must not
    # silently reset boot selection. Use the explicit flag for USB migration/recovery.
    if not (a.first_install or a.boot_factory):
        files = {offset:path for offset,path in files.items() if path.name!='ota_data_initial.bin'}
    if a.first_install:
        if not a.backup or not a.backup.is_file() or a.backup.stat().st_size != 16 * 1024 * 1024:
            raise SystemExit('First install requires --backup pointing to a complete 16 MiB flash backup.')
        print('Backup SHA256:', hashlib.file_digest(a.backup.open('rb'), 'sha256').hexdigest())
        private = ROOT / 'secrets' / a.device.upper()
        files.update({0x200000: private / 'system_nvs.bin', 0x206000: private / 'monitor_cfg.bin'})
    for offset, path in files.items():
        if not path.is_file():
            raise SystemExit(f'Missing image: {path}')
        if offset in (0x200000,0x206000) and path.stat().st_size != 0x6000:
            raise SystemExit('Unexpected NVS image size.')
        if offset not in (0,0x8000,0x10000,0x200000,0x206000,0x20d000):
            raise SystemExit('Unexpected flash offset; nothing was written.')
        if offset==0x20d000 and (path.name!='ota_data_initial.bin' or path.stat().st_size!=0x2000):
            raise SystemExit('Unexpected OTA metadata image.')
    base = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', a.port, '--baud', '460800']
    identity = subprocess.run(base + ['read-mac'], check=True, capture_output=True, text=True).stdout
    macs = re.findall(r'(?i)MAC:\s*([0-9a-f:]{17})', identity)
    if not any(mac.replace(':', '')[-6:].upper() == a.device.upper() for mac in macs):
        raise SystemExit('Board MAC does not match --device. Nothing was written.')
    command = base + ['write-flash'] + config['write_flash_args']
    for offset, path in sorted(files.items()):
        command.extend([hex(offset), str(path)])
    subprocess.run(command, check=True)
    print('Firmware written and verified by esptool. NVS ' + ('provisioned.' if a.first_install else 'preserved.'))


if __name__ == '__main__':
    main()
