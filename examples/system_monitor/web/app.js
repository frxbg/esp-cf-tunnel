'use strict';
const $ = id => document.getElementById(id);
let latest = null, session = '', sessionUntil = 0, fetching = false, scanTimer = 0;
let wifiEdited = false, tunnelEdited = false, rgbEdited = false, noticeTimer = 0;
const samples = [];
let scannedNetworks = [];
const pages = {
  overview: ['Overview', 'Live device health and connection status.'],
  io: ['Inputs / Outputs', 'Read pin levels and configure a small set of safe GPIOs.'],
  connection: ['Connection', 'Network, tunnel registration and ingress status.'],
  settings: ['Settings', 'Connect your device to a network and configure your tunnel.']
};
function navigate() {
  const page = Object.hasOwn(pages, location.hash.slice(1)) ? location.hash.slice(1) : 'overview';
  document.querySelectorAll('.page').forEach(el => { el.hidden = el.id !== page; });
  document.querySelectorAll('nav a').forEach(a => {
    if (a.dataset.page === page) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
  });
  $('page-title').textContent = pages[page][0]; $('page-description').textContent = pages[page][1];
  document.title = `${pages[page][0]} · ESP Monitor`;
  drawChart();
}
function notice(message, kind = 'info', persistent = false) {
  clearTimeout(noticeTimer); $('notice').textContent = message; $('notice').className = `notice ${kind}`; $('notice').hidden = false;
  if (!persistent) noticeTimer = setTimeout(() => { $('notice').hidden = true; }, 9000);
}
async function api(path, data) {
  const headers = {}; if (session) headers.Authorization = `Bearer ${session}`;
  if (data !== undefined) headers['Content-Type'] = 'application/json';
  const response = await fetch(path, {method: data === undefined ? 'GET' : 'POST', headers, body: data === undefined ? undefined : JSON.stringify(data), cache:'no-store', signal:AbortSignal.timeout(7000)});
  const result = await response.json();
  if (!response.ok) {
    if (response.status === 401 && path !== '/api/login') lock();
    throw new Error(result.error || `Request failed (${response.status}).`);
  }
  return result;
}
function activeSession() { return !!session && Date.now() < sessionUntil && latest?.admin_access; }
function lock() { session = ''; sessionUntil = 0; updateAccess(); }
function updateAccess() {
  if (session && Date.now() >= sessionUntil) { session = ''; sessionUntil = 0; }
  const unlocked = activeSession();
  $('wifi-fields').disabled = !unlocked; $('tunnel-fields').disabled = !unlocked;
  $('rgb-fields').disabled = !unlocked || !latest?.rgb?.available;
  $('login-form').hidden = unlocked; $('logout').hidden = !unlocked;
  $('lock-state').textContent = unlocked ? 'Unlocked · 10 minute session' : 'Locked';
  $('lock-state').className = `badge ${unlocked ? '' : 'neutral'}`;
  $('login-form').querySelector('button').disabled = !latest?.admin_access;
  $('device-password').disabled = !latest?.admin_access;
  $('setup-instructions').textContent = latest && !latest.admin_access ? 'This page origin cannot unlock the device. Open its configured hostname or local IP address.' : 'Use the device password to unlock settings over Cloudflare Tunnel, your local network or setup Wi-Fi. The session lasts 10 minutes.';
  $('io-access').textContent = unlocked ? 'Changes are temporary and reset to Disabled after a restart.' : 'Unlock Settings with the device password to configure pins.';
  document.querySelectorAll('.gpio-row').forEach(row => {
    row.querySelectorAll('select,button').forEach(el => { el.disabled = !unlocked; });
    row.querySelector('[data-level]').disabled = !unlocked || row.querySelector('[data-mode]').value !== '2';
  });
}
const kib = n => `${(n / 1024).toFixed(1)} KiB`;
const mib = n => `${(n / 1048576).toFixed(1)} MiB`;
function elapsed(ms) {
  const sec = Math.floor(ms / 1000), days = Math.floor(sec / 86400);
  return `${days ? `${days}d ` : ''}${String(Math.floor(sec / 3600) % 24).padStart(2,'0')}:${String(Math.floor(sec / 60) % 60).padStart(2,'0')}:${String(sec % 60).padStart(2,'0')}`;
}
function render(s) {
  document.querySelectorAll('[data-value]').forEach(el => { el.textContent = s[el.dataset.value] === '' ? '—' : (s[el.dataset.value] ?? '—'); });
  $('uptime').textContent = elapsed(s.uptime_ms); $('ram').textContent = kib(s.heap_free);
  $('ram-min').textContent = `Minimum: ${kib(s.heap_min)}`;
  $('temperature').textContent = s.temperature_c === null ? 'Unavailable' : `${s.temperature_c.toFixed(1)} °C`;
  $('rssi').textContent = s.wifi_associated ? `${s.rssi} dBm` : '—';
  $('signal-caption').textContent = s.wifi_associated ? s.ssid : 'No station connection';
  $('largest').textContent = `Largest free block: ${kib(s.heap_largest)}`;
  $('local-state').textContent = s.wifi_connected ? 'Connected' : (s.wifi_associated ? 'Obtaining IP' : s.wifi_connecting ? 'Connecting' : 'Disconnected');
  $('local-state').className = `badge ${s.wifi_connected ? '' : 'neutral'}`;
  $('wifi-detail').textContent = $('local-state').textContent;
  $('flash').textContent = mib(s.flash_bytes); $('psram').textContent = s.psram_total ? `${mib(s.psram_free)} / ${mib(s.psram_total)}` : 'Not enabled';
  $('cpu').textContent = `${s.cores} cores · ${s.cpu_mhz} MHz`; $('device-ip').textContent = s.ip || (s.ap_enabled ? s.ap_ip : '—');
  $('access-label').textContent = s.setup_access ? 'Setup Wi-Fi · local access' : s.admin_access ? 'Local network · password protected' : 'Cloudflare Tunnel · read only';
  $('channel').textContent = s.wifi_associated ? s.channel : '—';
  $('disconnect').textContent = s.disconnect_reason ? `Wi-Fi reason ${s.disconnect_reason}` : 'None';
  $('clock').textContent = s.clock_valid ? 'Valid time available' : 'Waiting for network time';
  $('token-detail').textContent = s.token_stored ? 'Stored on device' : 'Not configured';
  const online = s.tunnel_state === 'Online';
  for (const id of ['tunnel-state','tunnel-detail-state']) { $(id).textContent = s.tunnel_state || 'Waiting'; $(id).className = `badge ${online ? '' : 'neutral'}`; }
  $('internet-state').textContent = s.tunnel_registered ? 'Verified' : 'Not connected';
  $('internet-state').className = `badge ${s.tunnel_registered ? '' : 'neutral'}`;
  $('registration-detail').textContent = s.tunnel_registered ? 'Accepted' : 'Not registered';
  $('config-detail').textContent = s.tunnel_configured ? `Applied · ${s.config_version}` : 'Not applied';
  $('edge-detail').textContent = [s.edge_location,s.edge_ip].filter(Boolean).join(' · ') || '—';
  $('retry-detail').textContent = s.tunnel_retry_s ? `${s.tunnel_retry_s} s` : '—';
  $('h2-memory').textContent = `${kib(s.tunnel_h2_heap || 0)} · peak ${kib(s.tunnel_h2_peak || 0)}`;
  $('tunnel-stack').textContent = kib(s.tunnel_stack_min || 0);
  $('token-caption').textContent = s.token_stored ? 'Token stored. Leave blank to keep it.' : 'No token stored. This field is optional.';
  const resetNames = {1:'Power on',2:'External pin',3:'Software restart',4:'Panic',5:'Interrupt watchdog',6:'Task watchdog',7:'Other watchdog',8:'Deep sleep',9:'Brownout',10:'SDIO'};
  $('reset').textContent = resetNames[s.reset_reason] || `Code ${s.reset_reason}`;
  $('revision').textContent = `v${Math.floor(s.revision / 100)}.${s.revision % 100}`;
  $('ap-window').textContent = !s.ap_enabled ? 'Closed · hold BOOT for 3 seconds' : s.ap_remaining_s ? `${Math.ceil(s.ap_remaining_s / 60)} min remaining · ${s.ap_clients} client(s)` : `Available until Wi-Fi connects · ${s.ap_clients} client(s)`;
  if (!wifiEdited) $('ssid').value = s.ssid;
  if (!tunnelEdited) $('hostname').value = s.hostname;
  const rgb = s.rgb;
  $('rgb-state').textContent = !rgb?.available ? 'Unavailable' : rgb.on ? `On · ${rgb.brightness}%` : 'Off';
  $('rgb-state').className = `badge ${rgb?.on ? '' : 'neutral'}`;
  if (rgb && !rgbEdited) {
    $('rgb-color').value = '#' + [rgb.red,rgb.green,rgb.blue].map(v => v.toString(16).padStart(2,'0')).join('');
    $('rgb-brightness').value = rgb.brightness; $('rgb-brightness-value').textContent = `${rgb.brightness}%`; $('rgb-on').checked = rgb.on;
  }
  renderGpio(s.gpio); updateAccess(); drawChart();
}
function renderGpio(pins) {
  pins.forEach(pin => {
    if (![4,5,6,7].includes(pin.pin)) return;
    let row = $(`pin-${pin.pin}`);
    if (!row) {
      row = document.createElement('form'); row.className = 'gpio-row'; row.id = `pin-${pin.pin}`;
      // The markup contains only the fixed firmware GPIO allowlist, never user strings.
      row.innerHTML = `<strong>GPIO ${pin.pin}</strong><label>Measured level<output data-read>—</output></label><label>Mode<select data-mode><option value="0">Disabled</option><option value="1">Input</option><option value="2">Output</option></select></label><label>Output level<select data-level><option value="0">Low (0)</option><option value="1">High (1)</option></select></label><button type="submit">Apply</button>`;
      row.querySelector('[data-mode]').value = String(pin.mode);
      row.querySelector('[data-mode]').addEventListener('change', updateAccess);
      row.addEventListener('submit', async e => {
        e.preventDefault(); const mode = Number(row.querySelector('[data-mode]').value), value = Number(row.querySelector('[data-level]').value);
        await action(row.querySelector('button'), async () => { await api('/api/gpio',{pin:pin.pin,mode,value}); notice(`GPIO ${pin.pin} updated.`, 'success'); await refresh(); });
      });
      $('gpio-list').append(row);
    }
    row.querySelector('[data-read]').textContent = pin.mode === 0 ? 'Disabled' : `${pin.level ? 'High' : 'Low'} (${pin.level})`;
  });
}
function drawChart() {
  const canvas = $('memory-chart'); if ($('overview').hidden) return;
  const rect = canvas.getBoundingClientRect(); if (!rect.width) return;
  const ratio = window.devicePixelRatio || 1; canvas.width = Math.round(rect.width * ratio); canvas.height = Math.round(rect.height * ratio);
  const ctx = canvas.getContext('2d'); ctx.scale(ratio,ratio);
  const w = rect.width, h = rect.height, left = 43, right = w - 9, top = 15, bottom = h - 24;
  const max = Math.max(64, Math.ceil(Math.max(...samples.map(v => v.ram / 1024), 0) / 64) * 64);
  ctx.font = '10px Segoe UI, sans-serif'; ctx.fillStyle = '#798899'; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = top + (bottom-top) * i / 4;
    ctx.strokeStyle = '#e9eef2'; ctx.beginPath(); ctx.moveTo(left,y); ctx.lineTo(right,y); ctx.stroke();
    ctx.fillText(String(Math.round(max * (4-i) / 4)), 2, y+3);
  }
  ctx.fillText('KiB',2,h-7); ctx.fillText('−120 s',left,h-7); ctx.fillText('Now',right-22,h-7);
  $('chart-empty').hidden = !!samples.length;
  if (samples.length) {
    const points = samples.map((v,i) => [right-(samples.length-1-i)*(right-left)/59,bottom-(v.ram/1024/max)*(bottom-top)]);
    ctx.beginPath(); ctx.moveTo(points[0][0],bottom); points.forEach(([x,y]) => ctx.lineTo(x,y)); ctx.lineTo(right,bottom); ctx.closePath(); ctx.fillStyle='#e9f5f6'; ctx.fill();
    ctx.beginPath(); points.forEach(([x,y],i) => { if (i) ctx.lineTo(x,y); else ctx.moveTo(x,y); }); ctx.strokeStyle='#087e8b'; ctx.lineWidth=2; ctx.stroke();
    const last = points.at(-1); ctx.beginPath(); ctx.arc(last[0],last[1],3,0,Math.PI*2); ctx.fillStyle='#087e8b'; ctx.fill();
  }
}
async function refresh() {
  if (fetching) return; fetching = true;
  try {
    const s = await api('/api/status');
    if (latest && s.uptime_ms < latest.uptime_ms) { samples.length = 0; lock(); }
    latest = s; samples.push({ram:s.heap_free}); if (samples.length > 60) samples.shift(); render(s);
    $('last-check').textContent = `Checked ${new Date().toLocaleTimeString('en-GB')}`;
    if ($('notice').dataset.offline) { $('notice').hidden=true; delete $('notice').dataset.offline; }
  } catch (_) {
    $('last-check').textContent = 'Device unreachable';
    notice('The device is unreachable. Measurements are paused and may be out of date. Check your Wi-Fi connection, then refresh.', 'error', true); $('notice').dataset.offline='1';
  } finally { fetching = false; }
}
async function action(button, work) {
  button.disabled = true;
  try { await work(); } catch (err) { notice(err.name === 'TimeoutError' ? 'The request timed out. Check the connection before trying again.' : err.message, 'error'); }
  finally { button.disabled = false; updateAccess(); }
}
$('login-form').addEventListener('submit', e => { e.preventDefault(); action(e.submitter, async () => {
  const password = $('device-password').value; $('device-password').value = '';
  const result = await api('/api/login',{password}); session = result.session; sessionUntil = Date.now() + result.expires_in * 1000; updateAccess(); notice('Settings unlocked for 10 minutes.', 'success');
}); });
$('logout').addEventListener('click', () => action($('logout'),async () => { try { await api('/api/logout',{}); } finally { lock(); } notice('Settings locked.'); }));
$('wifi-form').addEventListener('input', () => { wifiEdited = true; });
$('tunnel-form').addEventListener('input', () => { tunnelEdited = true; });
$('rgb-form').addEventListener('input', () => { rgbEdited = true; $('rgb-brightness-value').textContent = `${$('rgb-brightness').value}%`; });
async function saveRgb() {
  const hex=$('rgb-color').value;
  await api('/api/rgb',{on:$('rgb-on').checked,red:parseInt(hex.slice(1,3),16),green:parseInt(hex.slice(3,5),16),blue:parseInt(hex.slice(5,7),16),brightness:Number($('rgb-brightness').value)});
  rgbEdited=false; await refresh(); notice('RGB LED updated.', 'success');
}
$('rgb-form').addEventListener('submit', e => { e.preventDefault(); action(e.submitter,saveRgb); });
document.querySelectorAll('[data-rgb]').forEach(button => button.addEventListener('click', () => action(button,async () => {
  const preset=button.dataset.rgb; $('rgb-on').checked=preset!=='off';
  if (preset!=='off') { $('rgb-color').value={red:'#ff0000',green:'#00ff00',blue:'#0000ff'}[preset]; if (!$('rgb-brightness').valueAsNumber) $('rgb-brightness').value=20; }
  rgbEdited=true; await saveRgb();
})));
$('open-network').addEventListener('change', () => { $('wifi-password').disabled = $('open-network').checked; if ($('open-network').checked) $('wifi-password').value=''; });
$('clear-token').addEventListener('change', () => { $('tunnel-token').disabled = $('clear-token').checked; if ($('clear-token').checked) $('tunnel-token').value=''; });
$('wifi-form').addEventListener('submit', e => { e.preventDefault(); action(e.submitter,async () => {
  const settings = {ssid:$('ssid').value,password:$('wifi-password').value,open:$('open-network').checked};
  await api('/api/wifi',settings); $('wifi-password').value=''; wifiEdited=false;
  notice('Wi-Fi settings saved. The device is connecting; the setup radio may change channel. Rejoin the setup network if needed.', 'success',true);
}); });
$('tunnel-form').addEventListener('submit', e => { e.preventDefault(); action(e.submitter,async () => {
  await api('/api/tunnel',{token:$('tunnel-token').value.trim(),hostname:$('hostname').value.trim(),clear:$('clear-token').checked});
  $('tunnel-token').value=''; $('clear-token').checked=false; $('tunnel-token').disabled=false; tunnelEdited=false;
  notice('Tunnel settings saved. Check Connection for registration and ingress status.', 'success'); await refresh();
}); });
$('scan').addEventListener('click', () => action($('scan'), async () => {
  clearTimeout(scanTimer); $('networks').disabled=true; await api('/api/scan',{}); $('scan-status').textContent='Scanning 2.4 GHz networks…';
  let attempts = 0;
  async function pollScan() {
    try {
      const result = await api('/api/scan');
      if (result.busy && ++attempts < 15) { scanTimer=setTimeout(pollScan,1000); return; }
      scannedNetworks=result.networks;
      $('networks').replaceChildren();
      scannedNetworks.forEach((network,index) => {
        const option=document.createElement('option'); option.value=String(index);
        option.textContent=`${network.ssid || '(Hidden network)'} · ${network.rssi} dBm · ${network.secure ? 'Secured' : 'Open'} · CH ${network.channel}`;
        option.disabled=!network.ssid; $('networks').append(option);
      });
      $('networks').selectedIndex=-1; $('networks').disabled=false;
      $('network-list-label').hidden=!scannedNetworks.length;
      $('scan-status').textContent=result.busy ? 'Scan is taking longer than expected. Try again shortly.' : scannedNetworks.length ? `${scannedNetworks.length} network(s) found. Choose a network below, then Save and connect.` : 'No networks found. Try scanning again or enter a hidden network manually.';
    } catch (err) { $('scan-status').textContent=err.message; }
  }
  scanTimer=setTimeout(pollScan,1000);
}));
$('networks').addEventListener('change', () => {
  if($('networks').selectedIndex<0 || !activeSession()) return;
  const network=scannedNetworks[Number($('networks').value)];
  if(!network?.ssid) return;
  if($('ssid').value!==network.ssid) $('wifi-password').value='';
  $('ssid').value=network.ssid; $('open-network').checked=!network.secure;
  $('wifi-password').disabled=!network.secure; wifiEdited=true;
  $('scan-status').textContent=`Selected ${network.ssid}. ${network.secure ? 'Enter the Wi-Fi password if needed, then' : 'This is an open network;'} select Save and connect.`;
});
$('refresh').addEventListener('click', refresh);
window.addEventListener('hashchange', navigate); window.addEventListener('resize', drawChart);
document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(); });
navigate(); refresh(); setInterval(() => { updateAccess(); if (!document.hidden) refresh(); },2000);
