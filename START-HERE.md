# PhoneMic — start here

Use your phone as a microphone for this PC. Plain steps, no jargon.

You set this up **once**. After that: open PhoneMic on the PC, tap **Start** on the phone, done.

---

## One-time setup

### 1. Get PhoneMic on the PC
- **Windows:** download `PhoneMic.exe` from the project's **Releases** page and put it
  anywhere (or next to `Start-PhoneMic.bat` in this folder). Double-click it.
  - If Windows asks to allow PhoneMic through the firewall, click **Allow** (private networks).
- **Linux:** download `PhoneMic-linux-x64.tar.gz` from Releases, unpack, run `./PhoneMic-linux`.
  Or run `./start-phonemic.sh` in this folder to build it (see docs/BUILDING.md).

### 2. Make it a real microphone
- **Windows:** in the PhoneMic window click **Set up microphone (VB-CABLE)**, then
  click **Install Driver** in the installer, then **reboot**.
- **Linux:** nothing to do; PhoneMic creates "PhoneMic Microphone" automatically.

### 3. Put the app on your phone (or skip it)
- **Android:** install `PhoneMic.apk` from Releases (allow "install unknown apps" when asked).
- **No app / iPhone:** tick **Browser mic** in the PC window and scan the QR code
  with the phone's camera. The first time, the browser warns the connection
  isn't private (it's your own PC's certificate): tap **Advanced → Proceed**.

---

## Every time you use it

1. Open **PhoneMic** on the PC.
2. On the phone (same Wi-Fi), open **PhoneMic**, tap the **🔍 search icon** to find the
   PC (or type the address shown in the PC window), then tap **Start**.
3. In **Discord / Zoom / OBS**, choose the microphone:
   - Windows: **CABLE Output (VB-Audio Virtual Cable)**
   - Linux: **PhoneMic Microphone**

The phone keeps streaming with the screen off. Stop it from the app or the notification.

---

## If something's off

The phone and the PC both tell you what's wrong. The most common cases:

| You see | Do this |
|---|---|
| Phone: *"Sending, but the PC isn't answering"* | Check the address. Allow PhoneMic through the PC's firewall (Windows Defender Firewall → Allow an app). Some guest/public Wi-Fi blocks devices from talking to each other. |
| Phone: *"No PC found"* | Make sure PhoneMic is open on the PC and both are on the same Wi-Fi; or type the address shown on the PC. |
| Phone: *"The PIN doesn't match"* | Type the same PIN on both. (A PIN is optional; leave both empty if you don't need encryption.) |
| PC: *"PhoneMic is already running"* | Close the other PhoneMic window, or choose another port and click Retry. |
| No "CABLE Output" in Discord | VB-CABLE isn't installed yet or needs a reboot: redo step 2. |
| Wi-Fi is bad or blocked | Use the cable: on the phone enable **Developer options → USB debugging**, tick **Stream over USB** in the PC window (needs Android *platform-tools*, i.e. `adb`), plug in, allow debugging, turn on **USB cable** in the phone app and tap Start. |
| Audio is choppy | Pick a bigger **buffer** in the PC window (e.g. "Stable Wi-Fi (80 ms)"), or move closer to the router. |
