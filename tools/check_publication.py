"""Check the publishable Git inventory; report paths/rule names, never secrets.

Use --staged immediately before committing. Requires Git, not ESP-IDF.
This project-specific check complements a full secret scanner such as Gitleaks.
"""
import argparse
import base64
import json
from pathlib import Path, PurePosixPath
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BLOCKED_DIRS = {'.cache', 'secrets', 'dist', '.venv', 'managed_components', '__pycache__', '.vscode', '.idea'}
BLOCKED_SUFFIXES = {'.elf', '.map', '.log', '.key', '.p12', '.pfx', '.exe', '.o', '.obj', '.a', '.lib', '.zip', '.csv', '.pyc'}
SYNTHETIC_TOKEN_FILES = {'tests/fixtures/reference_vectors.h', 'tests/host/test_core.c'}
REQUIRED = {'LICENSE', 'README.md', 'SECURITY.md', 'THIRD_PARTY_NOTICES',
    'examples/system_monitor/partitions.csv',
    'components/esp_cf_tunnel/LICENSE', 'components/esp_cf_tunnel/README.md',
    'components/esp_cf_tunnel/THIRD_PARTY_NOTICES',
    'components/esp_cf_tunnel/third_party/nghttp2/COPYING',
    'components/esp_cf_tunnel/third_party/cjson/LICENSE',
    'components/esp_cf_tunnel/certs/LICENSE-cloudflared'}


def inventory(staged=False):
    args = ['git', '-C', str(ROOT), 'ls-files', '-z', '--cached']
    if not staged:
        args += ['--others', '--exclude-standard']
    paths = sorted(set(subprocess.check_output(args).decode('utf-8').strip('\0').split('\0')) - {''})
    files = {}
    for name in paths:
        if staged:
            data = subprocess.check_output(['git', '-C', str(ROOT), 'show', ':' + name])
        else:
            path = ROOT / name
            if not path.is_file() or path.is_symlink():
                raise ValueError(f'Not a regular publication file: {name}')
            data = path.read_bytes()
        files[name] = data
    return files


def check(files):
    errors = []
    for missing in sorted(REQUIRED - files.keys()):
        errors.append((missing, 'required publication file missing'))
    for name, data in files.items():
        path = PurePosixPath(name)
        if any(p in BLOCKED_DIRS or p.startswith('build') for p in path.parts[:-1]):
            errors.append((name, 'private or generated directory'))
        if path.name in {'sdkconfig', 'sdkconfig.old', 'device-access.json'} or path.name.startswith('.env'):
            errors.append((name, 'local configuration'))
        if path.suffix.lower() in BLOCKED_SUFFIXES and name != 'examples/system_monitor/partitions.csv':
            errors.append((name, 'private/generated file type'))
        if path.suffix == '.bin' and not (name.startswith('tests/fixtures/rpc/') and len(data) <= 16384):
            errors.append((name, 'binary outside bounded synthetic fixtures'))
        if len(data) > 1024 * 1024:
            errors.append((name, 'file exceeds source budget'))
        if re.search(rb'-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----', data):
            errors.append((name, 'private key'))
        if re.search(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})', data):
            errors.append((name, 'GitHub credential'))
        for match in re.finditer(rb'eyJ[A-Za-z0-9+/=]{60,}', data):
            try:
                obj = json.loads(base64.b64decode(match[0], validate=True))
                if isinstance(obj, dict) and {'a', 's', 't'} <= obj.keys() and name not in SYNTHETIC_TOKEN_FILES:
                    errors.append((name, 'named tunnel token'))
            except (ValueError, UnicodeError):
                pass
    if sum(map(len, files.values())) > 10 * 1024 * 1024:
        errors.append(('<repository>', 'source payload exceeds 10 MiB budget'))
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--staged', action='store_true')
    args = p.parse_args()
    files = inventory(args.staged)
    errors = check(files)
    for name, reason in errors:
        print(f'FAIL: {name}: {reason}')
    if errors:
        return 1
    print(f'PASS: {len(files)} publication files, {sum(map(len, files.values()))} bytes; no forbidden artifacts or recognized credentials.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
