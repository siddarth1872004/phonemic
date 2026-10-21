# PhoneMic — start here

Use your phone as a Bluetooth microphone for this PC. Plain steps, no jargon.

You set this up **once**. After that: open PhoneMic on the PC, tap **Start** on the phone, done.

---

## One-time setup

### 1. Get PhoneMic on the PC
- **Windows:** download `PhoneMic-windows-x64.zip` from the project's **Releases** page,
  unzip it anywhere and double-click `PhoneMic.exe` (keep the `driver` folder next to it).
- **Linux (any distro):** download `PhoneMic-x86_64.AppImage` from Releases, make it
  executable (right-click → Properties → *Allow executing*, or `chmod +x`) and run it.
  Debian/Ubuntu: install the `.deb`; Fedora/openSUSE: install the `.rpm`.
- **Mac:** download `PhoneMic-macos.zip`, unzip, and open `PhoneMic.app`
  (the first time: right-click → **Open**, and allow Bluetooth when asked).

### 2. Make it a real microphone
PhoneMic brings its own microphone driver; nothing else to download. (This is
the microphone apps record from. Bluetooth itself needs no driver: PhoneMic uses
the one your PC already has.)
- **Windows 11 (22H2 or newer):** in the PhoneMic window click **Install PhoneMic driver**.
  A window explains each step and asks before turning on Windows *test mode* (needed
  until the driver is signed by Microsoft; Secure Boot must be off). Restart when it
  says so.
- **Linux:** nothing to do; PhoneMic creates "PhoneMic Microphone" automatically.
  Tick **Keep it when PhoneMic is closed** so apps you opened earlier see it too.
- **Mac:** click **Install PhoneMic driver** and enter your password.

### 3. Put the app on your phone
- **Android:** install `PhoneMic.apk` from Releases (allow "install unknown apps" when asked).

### 4. Pair the phone with the PC
Turn Bluetooth on on both, then pair them once, like a pair of headphones: on the
phone, **Settings → Bluetooth → Pair new device** and pick the PC (the PhoneMic
window shows its Bluetooth name). Accept the code on both screens.

Then in the phone app tap **Choose your PC**, pick it from the list, and allow
*Nearby devices* when asked.

---

## Every time you use it

1. Open **PhoneMic** on the PC.
2. On the phone, open **PhoneMic** and tap **Start**. Bluetooth must be on;
   no Wi-Fi needed.
3. In **Discord / Zoom / OBS**, choose the microphone **PhoneMic Microphone**
   (Windows lists it as *Microphone (PhoneMic Microphone)*).
   Apps with no microphone setting use the computer's default: click
   **Make it the default microphone** in the PhoneMic window once.

The phone keeps streaming with the screen off, and reconnects by itself if the
link drops. Stop it from the app or the notification.

---

## If something's off

The phone and the PC both tell you what's wrong. The most common cases:

| You see | Do this |
|---|---|
| Phone: *"Can't reach the PC…"* | Open PhoneMic on the PC and check its Bluetooth card says it's ready. Make sure the phone is paired with *this* PC (step 4), and that you picked the right PC in the app. |
| Phone: *"The PC is busy"* | Another phone is streaming to the PC. Stop it there first. |
| PC: *"Bluetooth is off"* / *"No Bluetooth"* | Turn Bluetooth on in the PC's settings. On Linux, make sure the Bluetooth service (BlueZ) is running: `systemctl start bluetooth`. |
| Your PC isn't in the phone's list | It isn't paired yet: tap **Bluetooth settings** in the app and pair it (step 4). |
| No "PhoneMic Microphone" in Discord | The driver isn't installed yet or Windows needs a restart: redo step 2. On Linux, make sure PipeWire or PulseAudio is running. |
| Audio is choppy | Keep the phone within a few metres of the PC, or pick a bigger **buffer** in the PC window (e.g. "Stable (80 ms)"). |
