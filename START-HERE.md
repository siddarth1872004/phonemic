# PhoneMic — start here

Use your phone as a microphone for this PC, over Bluetooth, Wi-Fi or a USB cable.
Plain steps, no jargon.

You set this up **once**. After that: open PhoneMic on the PC, tap **Start** on the phone, done.

---

## One-time setup

### 1. Get PhoneMic on the PC
- **Windows:** download `PhoneMic-windows-x64.zip` from the project's **Releases** page,
  unzip it anywhere and double-click `PhoneMic.exe`.
- **Linux (any distro):** download `PhoneMic-x86_64.AppImage` from Releases, make it
  executable (right-click → Properties → *Allow executing*, or `chmod +x`) and run it.
  Debian/Ubuntu: install the `.deb`; Fedora/openSUSE: install the `.rpm`.

### 2. Make it a real microphone
PhoneMic installs no driver of its own.
- **Windows:** in the PhoneMic window click **Install VB-CABLE**. PhoneMic downloads
  VB-CABLE (a free, signed virtual audio cable) from vb-audio.com, checks it really
  comes from VB-Audio, and starts its installer; say **Yes** to Windows' prompt, then
  restart Windows. Optional: click **Rename to PhoneMic** so apps list
  *PhoneMic Microphone* instead of *CABLE Output* (**Restore VB-CABLE names** undoes it).
  PhoneMic sends the phone's audio into the cable by itself, and moves your speakers
  back if Windows ever makes the cable the default speaker.
- **Linux:** nothing to do; PhoneMic creates "PhoneMic Microphone" automatically.
  Tick **Keep it when PhoneMic is closed** so apps you opened earlier see it too.

### 3. Put the app on your phone
- **Android:** install `PhoneMic.apk` from Releases (allow "install unknown apps" when asked).

### 4. Choose how the phone connects
At the top of the phone app pick **Bluetooth**, **Wi-Fi** or **USB**. You can switch any time.

- **Bluetooth:** pair the phone with the PC once, like a pair of headphones: on the
  phone, **Settings → Bluetooth → Pair new device** and pick the PC (the PhoneMic
  window shows its Bluetooth name). Then tap **Choose your PC**, pick it, and allow
  *Nearby devices* when asked.
- **Wi-Fi:** put the phone on the same Wi-Fi as the PC and tap **Find your PC**.
  If it isn't found (guest networks often block this), tap **Enter address…** and
  type one of the addresses shown in the PC's Wi-Fi section.
  *Optional PIN:* type a PIN into the PC's Wi-Fi section, then tap **PIN** on the
  phone and enter the same one. Nobody else on the network can then use your mic.
- **USB:** turn on *USB debugging* on the phone (Settings → About phone → tap
  *Build number* 7 times, then Developer options → USB debugging). Tick
  **USB cable** in the PhoneMic window, plug in the cable and allow the PC when
  the phone asks. The PC needs `adb` (Android SDK platform-tools).

---

## Every time you use it

1. Open **PhoneMic** on the PC.
2. On the phone, open **PhoneMic** and tap **Start**.
3. In **Discord / Zoom / OBS**, choose the microphone **PhoneMic Microphone**
   (on Windows: *CABLE Output (VB-Audio Virtual Cable)*, or
   *PhoneMic Microphone (VB-Audio Virtual Cable)* once renamed).
   Apps with no microphone setting use the computer's default: click
   **Make it the default microphone** in the PhoneMic window once.

The phone keeps streaming with the screen off, and reconnects by itself if the
link drops. Stop it from the app or the notification.

---

## If something's off

The phone and the PC both tell you what's wrong. The most common cases:

| You see | Do this |
|---|---|
| Phone: *"Can't reach the PC…"* (Bluetooth) | Open PhoneMic on the PC and check its Bluetooth section says it's ready. Make sure the phone is paired with *this* PC, and that you picked the right PC in the app. |
| Phone: *"Can't reach the PC…"* (Wi-Fi) | Both on the same network? Is **Wi-Fi** ticked in the PhoneMic window? Allow PhoneMic through the PC's firewall if Windows asks. |
| Phone: *"Can't reach the PC over USB…"* | Tick **USB cable** in the PhoneMic window and check its USB line: it says if `adb` is missing or the phone hasn't allowed the PC yet. |
| Phone: *"The PC wants a different PIN"* | Tap **PIN** on the phone and enter the PIN set in the PC's Wi-Fi section. |
| Phone: *"…but the PC has none"* | The PC has no PIN: tap **PIN** on the phone and clear it. |
| Phone: *"The PC is busy"* | Another phone is streaming to the PC. Stop it there first. |
| PC: *"Bluetooth is off"* / *"No Bluetooth"* | Turn Bluetooth on in the PC's settings. On Linux, make sure the Bluetooth service (BlueZ) is running: `systemctl start bluetooth`. |
| Your PC isn't in the phone's Bluetooth list | It isn't paired yet: tap **Bluetooth settings** in the app and pair it. |
| No PhoneMic / CABLE microphone in Discord | Windows: VB-CABLE isn't installed yet or Windows needs a restart, so redo step 2. Linux: make sure PipeWire or PulseAudio is running. |
| Everything you hear on the PC goes into the mic | Windows made the cable your speaker. PhoneMic moves it back by itself; if not, pick your real speakers in Windows' sound settings. |
| Audio is choppy | On Bluetooth keep the phone within a few metres of the PC; or try USB or Wi-Fi; or pick a bigger **buffer** in the PC window (e.g. "Stable (80 ms)"). |
| Want a different look | Both apps have **Appearance**: OLED black, Dark or Light, and an accent colour. |
