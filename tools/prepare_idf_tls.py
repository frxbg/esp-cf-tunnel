"""Prepare/check a project-local ESP-TLS override. Never edits the input SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH_DIR = ROOT / 'patches/esp-idf-v6.1'
META = json.loads((PATCH_DIR / 'manifest.json').read_text(encoding='utf-8'))
PATCH = PATCH_DIR / '0001-async-connect-poll.patch'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verified_source(sdk):
    for directory, expected in ((sdk, META['idf_commit']),
            (sdk / 'components/lwip/lwip', META['lwip_commit'])):
        actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != expected:
            raise ValueError('SDK/lwIP revision does not match the patch manifest')
    subprocess.run(['git', '-C', str(sdk), 'diff', '--exit-code', 'HEAD', '--', 'components/esp-tls'],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['git', '-C', str(sdk / 'components/lwip/lwip'), 'diff', '--exit-code', 'HEAD', '--', 'src/api/sockets.c'],
                   check=True, stdout=subprocess.DEVNULL)
    source = (sdk / META['source']).read_text(encoding='utf-8').encode('utf-8')
    patch = PATCH.read_text(encoding='utf-8').encode('utf-8')
    if digest(source) != META['original_sha256'] or digest(patch) != META['patch_sha256']:
        raise ValueError('SDK source or patch SHA256 mismatch')
    return source


def patched_source(source):
    # Apply the actual checked-in diff to a disposable non-Git directory.
    with tempfile.TemporaryDirectory(prefix='cf-idf-patch-') as directory:
        target = Path(directory) / META['source']
        target.parent.mkdir(parents=True)
        target.write_bytes(source)
        subprocess.run(['git', 'apply', '--no-index', '--check', str(PATCH)], cwd=directory, check=True)
        subprocess.run(['git', 'apply', '--no-index', str(PATCH)], cwd=directory, check=True)
        result = target.read_text(encoding='utf-8').encode('utf-8')
    if digest(result) != META['patched_sha256']:
        raise ValueError('Patched source SHA256 mismatch')
    return result


def prepare(sdk, output, check_only=False):
    sdk, output = sdk.resolve(), output.resolve()
    if output == sdk or sdk in output.parents:
        raise ValueError('Output must be outside the input SDK')
    source = verified_source(sdk)
    patched = patched_source(source)
    names = subprocess.check_output(['git', '-C', str(sdk), 'ls-files', '-z', '--', 'components/esp-tls']).decode().strip('\0').split('\0')
    expected = {str(Path(name).relative_to('components/esp-tls')).replace('\\','/'):
                (sdk / name).read_bytes() for name in names}
    expected['esp_tls.c'] = patched
    if output.exists():
        actual = {str(p.relative_to(output)).replace('\\','/'): p.read_bytes() for p in output.rglob('*') if p.is_file()}
        if actual != expected:
            raise ValueError('Existing override differs; use a fresh output directory (nothing overwritten)')
    elif check_only:
        raise ValueError('Override missing; prepare it before --check')
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        # Build fully before publishing the output directory.
        with tempfile.TemporaryDirectory(prefix='cf-idf-component-', dir=output.parent) as staging:
            staged = Path(staging) / 'esp-tls'
            for name, data in expected.items():
                target = staged / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            shutil.copytree(staged, output)
    print(f'PASS: ESP-IDF {META["idf_version"]} patch v{META["patch_version"]}; override SHA256 {digest(patched)}')
    return patched


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf-path', type=Path, default=os.environ.get('IDF_PATH'))
    p.add_argument('--output', type=Path, required=True, help='New esp-tls component directory outside the SDK')
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    if not a.idf_path: p.error('Set IDF_PATH or use --idf-path')
    prepare(a.idf_path, a.output, a.check)


if __name__ == '__main__':
    main()
