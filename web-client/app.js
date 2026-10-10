// PhoneMic browser client. Streams the phone's microphone to the PhoneMic
// desktop app over a TLS WebSocket, using the same v2 packet format as the
// Android app (shared/include/phonemic/wire.h).
'use strict';

const PM = { MAGIC: 0x4d50, VERSION: 2, KIND_AUDIO: 0, CODEC_PCM16: 0, HEADER: 28 };
const STATES = {
  0: ['ok', 'Streaming to your PC'],
  1: ['err', 'PC requires a PIN'],
  2: ['err', 'PC has no PIN set'],
  3: ['err', 'PIN mismatch'],
  4: ['warn', 'PC is busy: another device is streaming'],
  5: ['err', 'PC does not support this audio format'],
};
const MAX_BUFFERED = 32 * 1024; // drop frames rather than let latency pile up

const $ = (id) => document.getElementById(id);
const els = {
  toggle: $('toggle'), mute: $('mute'), status: $('status'), dot: $('dot'), level: $('level'),
  stat: $('stat'), code: $('code'), pairCard: $('pairCard'), cleanup: $('cleanup'),
};

let running = false, muted = false;
let ctx = null, node = null, stream = null, ws = null, wakeLock = null;
let session = 0, seq = 0, sent = 0, dropped = 0, reconnectDelay = 1000, reconnectTimer = 0;
let everOpened = false, failures = 0;

// ---- pairing code: from the QR/link (#t=...) or typed; remembered per device ----
function readToken() {
  const m = /(?:^|[#&])t=([a-z0-9]+)/i.exec(location.hash);
  if (m) {
    try { localStorage.setItem('phonemic.token', m[1]); } catch (_) {}
    history.replaceState(null, '', location.pathname); // keep the code out of history/screenshots
    return m[1];
  }
  try { return localStorage.getItem('phonemic.token') || ''; } catch (_) { return ''; }
}
let token = readToken();
els.code.value = token;
els.pairCard.hidden = !!token;
try { els.cleanup.checked = localStorage.getItem('phonemic.cleanup') !== '0'; } catch (_) {}

function setStatus(text, kind) {
  els.status.textContent = text;
  els.dot.className = 'dot' + (kind ? ' ' + kind : '');
}

function randomSession() {
  const a = new Uint32Array(1);
  do { crypto.getRandomValues(a); } while (a[0] === 0);
  return a[0];
}

async function start() {
  token = (els.code.value || token).trim().toLowerCase();
  if (!token) {
    els.pairCard.hidden = false;
    setStatus('Enter the pairing code from your PC', 'warn');
    els.code.focus();
    return;
  }
  try { localStorage.setItem('phonemic.token', token); localStorage.setItem('phonemic.cleanup', els.cleanup.checked ? '1' : '0'); } catch (_) {}

  running = true;
  everOpened = false;
  failures = 0;
  reconnectDelay = 1000;
  els.toggle.textContent = 'Stop mic';
  els.toggle.classList.add('on');
  els.mute.hidden = false;
  els.cleanup.disabled = true;

  // The AudioContext must be created inside the tap (iOS Safari otherwise
  // leaves it suspended and records silence).
  try {
    ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  } catch (_) {
    ctx = new AudioContext({ latencyHint: 'interactive' });
  }
  try {
    await ctx.resume();
    const clean = els.cleanup.checked;
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: clean, noiseSuppression: clean, autoGainControl: clean },
      video: false,
    });
    let src;
    try {
      src = ctx.createMediaStreamSource(stream);
    } catch (_) {
      // Firefox refuses to mix a 48 kHz context with a mic at another rate:
      // use the device rate instead (the PC resamples; the rate is in every packet).
      await ctx.close();
      ctx = new AudioContext({ latencyHint: 'interactive' });
      await ctx.resume();
      src = ctx.createMediaStreamSource(stream);
    }
    await ctx.audioWorklet.addModule('pcm-worklet.js');
    node = new AudioWorkletNode(ctx, 'pcm-worklet', {
      numberOfInputs: 1, numberOfOutputs: 0,
      processorOptions: { frame: Math.round(ctx.sampleRate / 100) },
    });
    node.port.onmessage = (ev) => onFrame(ev.data);
    node.port.postMessage({ muted });
    src.connect(node);
  } catch (e) {
    fail(e && e.name === 'NotAllowedError' ? 'Microphone permission was denied' : 'Microphone error: ' + (e.message || e));
    return;
  }
  acquireWakeLock();
  connect();
}

function connect() {
  if (!running) return;
  session = randomSession();
  seq = 0;
  if (failures === 0) setStatus('Connecting…', 'warn');
  ws = new WebSocket(`wss://${location.host}/ws?t=${encodeURIComponent(token)}`);
  ws.binaryType = 'arraybuffer';
  let opened = false;
  ws.onopen = () => {
    opened = everOpened = true;
    failures = 0;
    reconnectDelay = 1000;
    setStatus('Connected — waiting for audio…', 'warn');
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') return;
    let msg; try { msg = JSON.parse(ev.data); } catch (_) { return; }
    const [kind, text] = STATES[msg.state] || ['warn', 'Unknown state'];
    setStatus(msg.state === 0 ? `${text} · PC buffer ${msg.buffer} ms` : text, kind);
  };
  ws.onclose = () => {
    ws = null;
    if (!running) return;
    if (!opened) {
      failures++;
      // The server refuses the WebSocket for a wrong code (HTTP 403), which JS
      // can't see directly; a page that loaded fine but can't connect almost
      // always means the code is wrong.
      if (!everOpened && failures >= 3) {
        els.pairCard.hidden = false;
        fail('Could not connect — check the pairing code shown on your PC');
        return;
      }
      setStatus('Could not connect — retrying…', 'err');
    } else {
      setStatus('Connection lost — reconnecting…', 'warn');
    }
    reconnectTimer = setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 10000);
  };
}

function onFrame(msg) {
  els.level.style.width = Math.min(100, msg.peak * 140) + '%';
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (ws.bufferedAmount > MAX_BUFFERED) { dropped++; return; }

  const pcm = msg.pcm, bytes = pcm.length * 2;
  const buf = new ArrayBuffer(PM.HEADER + bytes);
  const dv = new DataView(buf);
  dv.setUint16(0, PM.MAGIC, true);
  dv.setUint8(2, PM.VERSION);
  dv.setUint8(3, PM.KIND_AUDIO);
  dv.setUint8(4, PM.CODEC_PCM16);
  dv.setUint8(5, 0);
  dv.setUint16(6, bytes, true);
  dv.setUint32(8, session, true);
  dv.setUint32(12, seq, true);
  dv.setBigUint64(16, BigInt(Math.round(performance.now() * 1000)), true);
  dv.setUint32(24, ctx.sampleRate, true);
  for (let i = 0; i < pcm.length; i++) dv.setInt16(PM.HEADER + 2 * i, pcm[i], true);
  ws.send(buf);
  seq = (seq + 1) >>> 0;
  sent++;
  if ((sent & 31) === 0) {
    els.stat.textContent = `${sent} frames sent · ${ctx.sampleRate} Hz` + (dropped ? ` · ${dropped} dropped (slow network)` : '');
  }
}

function teardown() {
  running = false;
  clearTimeout(reconnectTimer);
  els.toggle.textContent = 'Start mic';
  els.toggle.classList.remove('on');
  els.mute.hidden = true;
  els.cleanup.disabled = false;
  els.level.style.width = '0%';
  if (node) { node.disconnect(); node = null; }
  if (stream) { stream.getTracks().forEach((t) => t.stop()); stream = null; }
  if (ctx) { ctx.close(); ctx = null; }
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
  if (wakeLock) { wakeLock.release().catch(() => {}); wakeLock = null; }
}

function stop() { teardown(); setStatus('Stopped', ''); }
// Keep the error visible (the old client immediately overwrote it with "stopped").
function fail(msg) { teardown(); setStatus(msg, 'err'); }

async function acquireWakeLock() {
  try {
    if (!('wakeLock' in navigator) || !running) return;
    wakeLock = await navigator.wakeLock.request('screen');
    wakeLock.addEventListener('release', () => { wakeLock = null; });
  } catch (_) {}
}
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && running && !wakeLock) acquireWakeLock();
});

els.toggle.onclick = () => (running ? stop() : start());
els.mute.onclick = () => {
  muted = !muted;
  els.mute.textContent = muted ? 'Unmute' : 'Mute';
  els.mute.classList.toggle('on', muted);
  if (node) node.port.postMessage({ muted });
};

if (!window.isSecureContext || !navigator.mediaDevices) {
  setStatus('Open this page via the https:// link shown in the PhoneMic app', 'err');
  els.toggle.disabled = true;
}
