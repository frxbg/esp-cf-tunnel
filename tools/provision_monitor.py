"""Create private first-install NVS images. Does not flash or print secrets.

Run in an activated ESP-IDF environment. Re-running keeps the existing password.
Normal firmware updates must NOT flash these factory provisioning images again.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import secrets
import string
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, help='Last 6 hex digits of station MAC')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9A-Fa-f]{6}', args.device):
        parser.error('--device must contain exactly 6 hex digits')
    directory = ROOT / 'secrets' / args.device.upper()
    directory.mkdir(parents=True, exist_ok=True)
    credentials = directory / 'device-access.json'
    ssid = f'ESP-Monitor-{args.device.upper()}'
    if credentials.exists():
        access = json.loads(credentials.read_text(encoding='utf-8'))
        if access['ssid'] != ssid or not re.fullmatch(r'[A-Za-z0-9]{20}', access['password']):
            raise SystemExit('Existing provisioning file is invalid; it was not overwritten.')
    else:
        access = dict(ssid=ssid, password=''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(20)),
                      url='http://192.168.4.1', note='Use this password for setup Wi-Fi and Settings unlock. Keep this file private.')
        with credentials.open('x', encoding='utf-8') as f:
            json.dump(access, f, indent=2)
            f.write('\n')
    generator = Path(os.environ['IDF_PATH']) / 'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'
    for name, rows in (
        ('monitor_cfg', [('monitor', 'namespace', '', ''), ('setup_pass', 'data', 'string', access['password'])]),
        ('system_nvs', [('factory', 'namespace', '', '')]),
    ):
        source = directory / f'{name}.csv'
        with source.open('w', newline='', encoding='utf-8') as f:
            writer = csv.writer(f); writer.writerow(['key', 'type', 'encoding', 'value']); writer.writerows(rows)
        try:
            subprocess.run([sys.executable, str(generator), 'generate', str(source), str(directory / f'{name}.bin'), '0x6000'], check=True, capture_output=True)
        except subprocess.CalledProcessError:
            raise SystemExit('NVS generation failed. No secret output was printed.') from None
        finally:
            source.unlink(missing_ok=True)
    print(f'Private device access file: {credentials}')
    print('NVS images ready for first installation only. Password was not logged.')


if __name__ == '__main__':
    main()
