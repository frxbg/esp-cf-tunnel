"""Authenticated OTA regression on a real monitor; never stores credentials.

Invalid updates write only the inactive slot. --upload installs the supplied app
and reboots. --cycles restarts the tunnel task without changing its credentials.
The password is read from a hidden terminal prompt, never a command argument.
"""
import argparse
import base64
import getpass
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.request


def run(a):
    base = a.url.rstrip('/')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    session = ''
    checks, cycles = [], []
    def request(path, data=None, expected=200, origin=None):
        headers = {'Origin': origin or base}
        if session: headers['Authorization'] = 'Bearer ' + session
        if data is not None: headers['Content-Type'] = 'application/json'
        payload = json.dumps(data,separators=(',',':')).encode() if data is not None else None
        assert payload is None or len(payload)<=2048
        try:
            response = opener.open(urllib.request.Request(base+path,payload,headers),timeout=8)
        except urllib.error.HTTPError as err:
            response = err
        with response:
            assert response.status==expected, f'{path}: expected {expected}, got {response.status}'
            return json.loads(response.read())
    def passed(name):
        checks.append(name); print('PASS:',name,flush=True)
    def safe(s):
        keys=('version','uptime_ms','heap_free','heap_min','heap_largest','tasks','reset_reason',
              'tunnel_state','tunnel_attempts','tunnel_failures','tunnel_stack_min','sntp_sync_count')
        return {**{k:s.get(k) for k in keys}, 'ota':s.get('ota')}
    def start(data, digest=None):
        return request('/api/ota/start',{'size':len(data),'sha256':digest or hashlib.sha256(data).hexdigest()})['id']
    def chunk(upload,offset,data,expected=200):
        return request('/api/ota/chunk',{'id':upload,'offset':offset,'data':base64.b64encode(data).decode()},expected)
    def send(upload,data):
        for offset in range(0,len(data),1408):
            end=min(offset+1408,len(data))
            assert chunk(upload,offset,data[offset:end])['received']==end
            if offset//1408%150==0: print(f'Upload {end*100//len(data)}%',flush=True)
    image=a.image.read_bytes()
    initial=request('/api/status'); assert initial['ota']['available']
    request('/api/ota/start',{'size':len(image),'sha256':hashlib.sha256(image).hexdigest()},401)
    passed('Unauthenticated OTA rejected')
    password=getpass.getpass('Device password (not recorded): ')
    session=request('/api/login',{'password':password})['session']
    request('/api/ota/start',{'size':initial['ota']['capacity']+1,'sha256':'0'*64},400)
    request('/api/ota/start',{'size':len(image),'sha256':'not-a-digest'},400)
    passed('Oversized image and malformed digest rejected')
    for name,offset in [('image magic',0),('target chip',12),('project identity',80)]:
        bad=bytearray(image[:1408]); bad[offset]^=255
        upload=start(image); chunk(upload,0,bad,400)
        assert not request('/api/status')['ota']['active']
        passed(f'Wrong {name} rejected before opening flash')
    upload=start(image)
    request('/api/ota/abort',{'id':upload},401,origin='http://example.invalid')
    chunk(upload,1,image[:1408],400)
    request('/api/ota/chunk',{'id':upload,'offset':0,'data':'!invalid!'},400)
    chunk(upload,0,image[:1],400)
    chunk(upload,0,image[:1408])
    chunk(upload,0,image[:1408],400)
    request('/api/ota/finish',{'id':upload},400)
    request('/api/ota/abort',{'id':upload})
    passed('Origin, encoding, prefix, offset, duplicate, incomplete and abort checks')
    short=image[:1000]
    for digest,name in [('0'*64,'SHA256 mismatch'),(hashlib.sha256(short).hexdigest(),'Truncated ESP image')]:
        upload=start(short,digest); chunk(upload,0,short)
        request('/api/ota/finish',{'id':upload},400)
        after=request('/api/status')
        assert not after['ota']['active'] and after['ota']['running']==initial['ota']['running']
        assert after['uptime_ms']>=initial['uptime_ms']
        passed(f'{name} rejected; running application unchanged')
    if a.upload:
        upload=start(image); send(upload,image)
        before=request('/api/status'); target=before['ota']['target']
        request('/api/ota/finish',{'id':upload}); session=''
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            time.sleep(1)
            try: after=request('/api/status')
            except (OSError,ValueError): continue
            if after['ota']['running']==target and after['uptime_ms']<before['uptime_ms']: break
        else: raise AssertionError('Updated application did not return in the target slot')
        assert after['token_stored']==initial['token_stored'] and after['hostname']==initial['hostname']
        assert after['ssid']==initial['ssid'] and after['reset_reason']==3
        session=request('/api/login',{'password':password})['session']
        passed('OTA booted target slot; device password, Wi-Fi and tunnel configuration retained')
    del password
    for i in range(a.cycles):
        before=request('/api/status')
        request('/api/tunnel/restart',{})
        # Restart is deferred by one second to finish the HTTP response.
        time.sleep(1.5); deadline=time.monotonic()+90; samples=0
        while time.monotonic()<deadline:
            state=request('/api/status'); samples+=1
            assert state['uptime_ms']>=before['uptime_ms'], 'Unexpected device reset'
            if state['tunnel_state']=='Online' and state['tunnel_connect']['at_ms']>before['uptime_ms']: break
            time.sleep(.25)
        else: raise AssertionError('Tunnel did not recover')
        time.sleep(1); state=request('/api/status')
        cycles.append(safe(state)); passed(f'Tunnel stop/start {i+1}: Online, {samples} responsive status polls, heap {state["heap_free"]}')
    final=request('/api/status'); request('/api/logout',{}); session=''
    report={'checks':checks,'initial':safe(initial),'final':safe(final),'cycles':cycles}
    if a.report: a.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f'PASS: {len(checks)} hardware checks',flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url',required=True)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--upload',action='store_true')
    p.add_argument('--cycles',type=int,default=0)
    p.add_argument('--report',type=Path)
    run(p.parse_args())
