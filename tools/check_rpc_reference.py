"""Rebuild and compare synthetic RPC fixtures, then run real Go/C RPC interop."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = 'f11dea9cb7079e90a982c1a2d5548ab40847fdcf'


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--go', default='go')
    p.add_argument('--client', type=Path, required=True, help='Built cf_rpc_tests executable')
    a = p.parse_args()
    reference = ROOT / '.cache/cloudflared'
    head = run('git', '-C', reference, 'rev-parse', 'HEAD', capture_output=True, text=True).stdout.strip()
    if head != PIN:
        raise SystemExit('Reference checkout does not match dependencies.lock.json')
    run('git', '-C', reference, 'diff', '--exit-code', 'HEAD', '--', 'tunnelrpc/proto', 'go.mod', 'go.sum')
    with tempfile.TemporaryDirectory(prefix='esp-cf-rpc-') as temp:
        temp = Path(temp)
        server = temp / ('rpc-reference.exe' if sys.platform == 'win32' else 'rpc-reference')
        run(a.go, 'build', '-o', server, '.', cwd=ROOT / 'tools/rpc_reference')
        generated = temp / 'fixtures'
        generated.mkdir()
        run(server, 'generate', generated)
        checked = ROOT / 'tests/fixtures/rpc'
        expected = {f.name: f.read_bytes() for f in checked.iterdir() if f.is_file()}
        actual = {f.name: f.read_bytes() for f in generated.iterdir() if f.is_file()}
        if actual != expected:
            raise SystemExit('Synthetic RPC fixture drift; inspect before updating checked-in fixtures')
        encoded = temp / 'c-encoded'
        encoded.mkdir()
        run(a.client.resolve(), 'emit', encoded)
        run(server, 'inspect', encoded)
        run(sys.executable, ROOT / 'tools/test_rpc_interop.py', '--client', a.client.resolve(), '--server', server)
    print('PASS: pinned schema, fixture byte comparison, C message decoding and Go RPC interoperability')


if __name__ == '__main__':
    main()
