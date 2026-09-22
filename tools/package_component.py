"""Create a reproducible source-only ESP-IDF component ZIP and SHA256 file."""
import hashlib
from pathlib import Path
import re
import subprocess
import zipfile

from check_publication import ROOT, check, inventory


def main():
    if subprocess.call(['git', '-C', str(ROOT), 'diff', '--quiet', '--', 'components/esp_cf_tunnel']):
        raise SystemExit('Stage intended component changes before packaging; the ZIP uses the Git index.')
    # Git blobs have canonical LF endings, independent of Windows checkout settings.
    files = inventory(staged=True)
    errors = check(files)
    if errors:
        raise SystemExit('Publication checks failed; run tools/check_publication.py for paths/rules.')
    prefix = 'components/esp_cf_tunnel/'
    manifest = files[prefix + 'idf_component.yml'].decode('utf-8')
    version = re.search(r'^version: "([0-9]+\.[0-9]+\.[0-9]+)"$', manifest, re.M)[1]
    destination = ROOT / 'dist'
    destination.mkdir(exist_ok=True)
    archive = destination / f'esp_cf_tunnel-{version}.zip'
    count = 0
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as out:
        for name, data in sorted(files.items()):
            if not name.startswith(prefix):
                continue
            entry = zipfile.ZipInfo('esp_cf_tunnel/' + name[len(prefix):], (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            out.writestr(entry, data, compresslevel=9)
            count += 1
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum = destination / 'SHA256SUMS'
    checksum.write_text(f'{digest}  {archive.name}\n', encoding='utf-8', newline='\n')
    print(f'Packaged {count} component files: {archive.name} ({archive.stat().st_size} bytes)')
    print(f'SHA256: {digest}')


if __name__ == '__main__':
    main()
