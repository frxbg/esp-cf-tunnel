"""Exercise an installed monitor on its setup Wi-Fi without driving GPIOs.

Only --exercise-token-store writes a synthetic token, and only when no token or
hostname is already stored. It clears that test value in a finally block.
Never prints device passwords, sessions, or token bodies.
"""
import argparse
import base64
import json
from pathlib import Path
import secrets
import time
import urllib.error
import urllib.request


def run(access_file, report_path, exercise_token=False, url=None, exercise_rgb=False):
    access = json.loads(Path(access_file).read_text(encoding='utf-8'))
    base = (url or access['url']).rstrip('/')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    session = ''
    checks = []

    def request(path, data=None, *, expected=200, raw=None, extra=None):
        headers = {'Origin': base}
        if session:
            headers['Authorization'] = 'Bearer ' + session
        if data is not None or raw is not None:
            headers['Content-Type'] = 'application/json'
        headers.update(extra or {})
        payload = raw if raw is not None else (json.dumps(data).encode() if data is not None else None)
        req = urllib.request.Request(base + path, payload, headers)
        try:
            response = opener.open(req, timeout=4)
        except urllib.error.HTTPError as err:
            response = err
        with response:
            body = response.read()
            assert response.status == expected, f'{path}: expected {expected}, received {response.status}'
            if path.startswith('/api/'):
                assert 'no-store' in response.headers.get('Cache-Control', '')
                return json.loads(body)
            assert response.headers.get('X-Content-Type-Options') == 'nosniff'
            assert "frame-ancestors 'none'" in response.headers.get('Content-Security-Policy', '')
            return body

    def passed(name):
        checks.append(name)
        print('PASS:', name, flush=True)

    initial = request('/api/status')
    Path(report_path).write_text(json.dumps({'initial':initial},indent=2)+'\n',encoding='utf-8')
    assert initial['admin_access'], 'Local IP must allow password authentication on AP and LAN'
    assert initial['flash_bytes'] == 16777216, 'Flash size must be 16 MiB'
    assert initial['heap_free'] > 32768 and initial['uptime_ms'] > 0
    assert all(p['mode'] == 0 and p['level'] == -1 for p in initial['gpio'])
    passed('Real device telemetry; all GPIOs disabled')
    for path in ('/', '/style.css', '/app.js', '/favicon.svg'):
        asset = request(path)
        assert len(asset) > 100
        assert not any('\u0400' <= c <= '\u04ff' for c in asset.decode('utf-8'))
    passed('All four assets served with security headers and English text')
    request('/api/gpio', {'pin':4,'mode':2,'value':1}, expected=401)
    request('/api/rgb', {'on':True,'red':255,'green':0,'blue':0,'brightness':20}, expected=401)
    request('/api/login', {'password': access['password']}, expected=403, extra={'Origin':'http://example.invalid'})
    request('/api/login', {'password':'incorrect-password'}, expected=401)
    passed('Unauthenticated GPIO and cross-origin login rejected')
    session = request('/api/login', {'password':access['password']})['session']
    assert len(session) == 64
    passed('Device password unlocks an authenticated session')
    request('/api/gpio', {'pin':48,'mode':2,'value':1}, expected=400)
    request('/api/gpio', {'pin':4.5,'mode':1,'value':0}, expected=400)
    request('/api/gpio', {'pin':4,'mode':9,'value':0}, expected=400)
    request('/api/gpio', raw=b'{"pin":4,"pin":5,"mode":1,"value":0}', expected=400)
    request('/api/gpio', raw=b'{"pin":4,"mode":1,"value":0,"extra":true}', expected=400)
    request('/api/gpio', raw=b'{"pin":4,"mode":1,"value":0}\x00', expected=400)
    try:
        request('/api/gpio', raw=b'x'*2049, expected=400)
    except ConnectionResetError:
        # Closing a socket with unread excess input can discard its response.
        # The following fresh request must prove that the device stayed alive.
        pass
    request('/api/gpio', {'pin':4,'mode':2,'value':1}, extra={'Host':'rebound.example'}, expected=401)
    after_rejections = request('/api/status')
    assert after_rejections['gpio'] == initial['gpio']
    assert after_rejections['uptime_ms'] >= initial['uptime_ms']
    passed('Reserved pins, fractional values, modes, duplicates, NUL, oversized bodies and rebinding rejected without driving pins')
    request('/api/wifi', {'ssid':'','password':'12345678','open':False}, expected=400)
    request('/api/wifi', {'ssid':'x','password':'short','open':False}, expected=400)
    request('/api/wifi', {'ssid':'x','password':'12345678','open':'false'}, expected=400)
    request('/api/tunnel', {'token':'not-a-token','hostname':'device.example.com','clear':False}, expected=400)
    request('/api/tunnel', {'token':'','hostname':'https://example.com/path','clear':False}, expected=400)
    passed('Invalid Wi-Fi credentials and tunnel settings rejected')
    for invalid in [dict(on=True,red=256,green=0,blue=0,brightness=20),
                    dict(on=True,red=0.5,green=0,blue=0,brightness=20),
                    dict(on='true',red=0,green=0,blue=0,brightness=20),
                    dict(on=True,red=0,green=0,blue=0,brightness=101)]:
        request('/api/rgb',invalid,expected=400)
    assert request('/api/status')['rgb'] == initial['rgb']
    passed('Invalid RGB values rejected without changing the LED')
    if exercise_rgb:
        for color in [(255,0,0),(0,255,0),(0,0,255)]:
            value=dict(on=True,red=color[0],green=color[1],blue=color[2],brightness=20)
            request('/api/rgb',value)
            state=request('/api/status')['rgb']
            assert all(state[k]==v for k,v in value.items())
            time.sleep(.3)
        request('/api/rgb',dict(on=False,red=0,green=0,blue=255,brightness=20))
        assert not request('/api/status')['rgb']['on']
        passed('RGB red/green/blue/off commands accepted by RMT driver and reflected in telemetry')
    request('/api/scan', {})
    for _ in range(12):
        time.sleep(.5)
        scan = request('/api/scan')
        if not scan['busy']:
            break
    assert not scan['busy'] and len(scan['networks']) <= 12
    passed(f'Asynchronous Wi-Fi scan completed ({len(scan["networks"])} results)')
    if exercise_token and not initial['token_stored'] and not initial['hostname']:
        value = {'a':'1'*32,'t':'12345678-1234-4234-8234-123456789abc','s':base64.b64encode(secrets.token_bytes(36)).decode()}
        synthetic = base64.b64encode(json.dumps(value,separators=(',',':')).encode()).decode()
        try:
            request('/api/tunnel', {'token':synthetic,'hostname':'device.example.com','clear':False})
            state = request('/api/status')
            assert state['token_stored'] and not state['tunnel_registered']
            assert synthetic not in json.dumps(state) and access['password'] not in json.dumps(state)
            request('/api/tunnel', {'token':'','hostname':'device.example.com','clear':False})
            assert request('/api/status')['token_stored']
        finally:
            request('/api/tunnel', {'token':'','hostname':'','clear':True})
        assert not request('/api/status')['token_stored']
        passed('Synthetic token validated, saved, kept on blank input, never exposed, and cleared')
    request('/api/logout', {})
    request('/api/scan', expected=401)
    passed('Logout invalidates the previous session')
    final = request('/api/status')
    assert final['gpio'] == initial['gpio']
    report = {'checks':checks, 'initial':initial, 'final':final,
              'not_run':['Station Wi-Fi provisioning with real network credentials', 'GPIO output drive', 'Live Cloudflare transport']}
    Path(report_path).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f'PASS: {len(checks)} API test groups. Report: {report_path}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--access-file',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--exercise-token-store',action='store_true')
    p.add_argument('--exercise-rgb',action='store_true')
    p.add_argument('--url',help='Override setup URL to test the current LAN IP')
    a = p.parse_args()
    run(a.access_file,a.report,a.exercise_token_store,a.url,a.exercise_rgb)
