# Browser mic (no app install)

Any phone (Android or iPhone) can be the microphone through its web browser.
The page is served **by the desktop app itself**, and its audio goes through the
same pipeline as the native app: Voice Focus, voice changer, gain, virtual
microphone.

## Use it

1. In the PhoneMic window, tick **Browser mic**.
2. Scan the QR code with the phone's camera (or open the link shown).
3. The first time, the browser warns that the connection isn't private. That's
   expected: it's your PC's own self-signed certificate. Tap **Advanced →
   Proceed**. The certificate is kept, so this happens once per phone.
4. Tap **Start mic** and allow the microphone.

## How it works

```
phone browser                                   PhoneMic desktop app
getUserMedia → AudioWorklet (10 ms PCM16)       HTTPS: index.html, app.js, worklet (embedded)
  → v2 AUDIO packet (actual sample rate)  ───►  wss /ws?t=<pairing code>
  → WebSocket (drops frames if congested)        → Receiver::submit() → same pipeline
  ◄─── {"state":0,"buffer":40,...} status ─────
```

- **Why HTTPS:** browsers only allow microphone access in a secure context.
- **Pairing code:** a random 8-character code in the link/QR (`#t=…`, a URL
  fragment, so it's never sent in plain HTTP requests or logs) must be
  presented to open the audio WebSocket. Compared in constant time; "New code"
  in the app invalidates old links. The Origin header is checked, and the pages
  use a strict Content-Security-Policy.
- **Certificate:** ECDSA P-256, generated on first use and stored in the
  per-user config directory (`web-cert.pem`, `web-key.pem` with 0600 on Linux).
- **Sample rate:** the page tries 48 kHz; if the browser can't (Firefox with a
  44.1 kHz mic), it uses the device rate and the PC resamples.
- **Robustness:** the AudioContext is created inside the tap (required by iOS),
  the screen is kept awake with the Wake Lock API, frames are dropped rather
  than queued when the network is congested (TCP would otherwise let latency
  grow without bound), and the page reconnects automatically.

## Native app vs browser

| | Android app | Browser |
|---|---|---|
| Install | APK | none |
| iPhone | ✗ | ✓ |
| Screen off | keeps streaming | most browsers pause the mic |
| Transport | UDP (loss-tolerant) | TCP (WebSocket) |
| Latency | lowest | slightly higher |
| Encryption | optional PIN | always (TLS) |
