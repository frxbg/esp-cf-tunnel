"""Build the monitor in an activated ESP-IDF environment, without flashing."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile', choices=('yd_s3', 'no_psram'), default='yd_s3')
    p.add_argument('--tls-override', type=Path, help='Explicit project-local patched esp-tls component')
    args = p.parse_args()
    if not os.environ.get('IDF_PATH'):
        p.error('Activate ESP-IDF v6.1 first.')
    if args.tls_override:
        from prepare_idf_tls import prepare
        prepare(Path(os.environ['IDF_PATH']), args.tls_override, check_only=True)
    project = ROOT / 'examples/system_monitor'
    build = ROOT / f'build-monitor-{args.profile}'
    defaults = ';'.join(str(x) for x in (project / 'sdkconfig.defaults', project / 'profiles' / f'{args.profile}.defaults'))
    return subprocess.call([sys.executable, str(Path(os.environ['IDF_PATH']) / 'tools/idf.py'),
        '-C', str(project), '-B', str(build), '-D', 'IDF_TARGET=esp32s3',
        '-D', f'SDKCONFIG={build / "sdkconfig"}', '-D', f'SDKCONFIG_DEFAULTS={defaults}',
        '-D', f'CF_IDF_TLS_OVERRIDE={args.tls_override.resolve() if args.tls_override else ""}', 'build'],
        env={**os.environ, 'PYTHONUTF8': '1'})


if __name__ == '__main__':
    sys.exit(main())
