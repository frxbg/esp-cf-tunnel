"""Connect the real C RPC client to cloudflared's Go Cap'n Proto RPC server.

Only fixed synthetic credentials, never accesses the device or secrets folder.
Pipes fragment every transfer; a stalled/failed peer is killed after 20 seconds.
"""
import argparse
from pathlib import Path
import subprocess
import threading


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--server', type=Path, required=True)
    args = p.parse_args()
    client = subprocess.Popen([str(args.client.resolve()), 'peer'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    server = subprocess.Popen([str(args.server.resolve()), 'serve'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    failures = []

    def pump(source, target):
        try:
            while data := source.read(1):
                target.write(data)
                target.flush()
        except (BrokenPipeError, OSError) as exc:
            failures.append(type(exc).__name__)
        finally:
            target.close()

    threads = [threading.Thread(target=pump, args=(client.stdout, server.stdin), daemon=True),
               threading.Thread(target=pump, args=(server.stdout, client.stdin), daemon=True)]
    for thread in threads:
        thread.start()
    try:
        client.wait(timeout=20)
        server.wait(timeout=20)
    finally:
        for proc in (client, server):
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    for thread in threads:
        thread.join(timeout=1)
    for proc in (client, server):
        print(proc.stderr.read().decode(errors='replace').strip())
    if client.returncode or server.returncode or failures:
        raise SystemExit(f'Interop failed: client={client.returncode}, server={server.returncode}, pipes={failures}')
    print('PASS: bidirectional RPC interoperability with one-byte transport fragments')


if __name__ == '__main__':
    main()
