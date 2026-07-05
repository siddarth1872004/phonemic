# PhoneMic — start here

Use your Android phone as a microphone for this Windows PC. Plain steps, no jargon.

You set this up **once**. After that it's: double-click, tap Start on the phone, done.

---

## One-time setup (about 30–40 min, mostly downloads)

### 1. Install VB-CABLE (makes your phone show up as a real mic)
- Go to **vb-audio.com/Cable**, download **VB-CABLE**, unzip, right-click
  `VBCABLE_Setup_x64.exe` → **Run as administrator** → Install.
- **Reboot** the PC.

*(Without this, PhoneMic still works but the sound just comes out your speakers
instead of being usable as a mic in apps.)*

### 2. Put the app on your phone (one time)
- Install **Android Studio** (developer.android.com/studio) — pick the Standard
  setup. In its **SDK Manager** add: **NDK 26.1.10909125** and **CMake 3.22.1**
  (SDK Tools tab → tick "Show Package Details").
- Open the folder **`C:\Users\Siddarth\phonemic\phone-app`** in Android Studio.
- Plug your phone in with USB (enable **Developer Options → USB debugging** first:
  tap "Build number" 7 times in Settings → About phone).
- Press the green **Run ▶** button → pick your phone. It installs the PhoneMic app.

*(Detailed version with troubleshooting: [docs/TIER2-3-HANDOFF.md](docs/TIER2-3-HANDOFF.md).)*

---

## Every time you want to use it

1. **Double-click `Start-PhoneMic.bat`** (in this folder). A window opens and
   shows your PC's IP, e.g. `192.168.29.26`.
2. On your **phone** (same Wi-Fi), open the **PhoneMic** app → type that **IP**,
   tap **Start**, allow the mic.
3. In **Discord / Zoom / OBS**, choose the microphone named
   **"CABLE Output (VB-Audio Virtual Cable)"**.

That's it — your phone is now the mic. Close the black window to stop.

---

## If something's off
- **App says it can't connect:** phone must be on the **same Wi-Fi** (not mobile
  data), and Windows may ask to allow the app through the firewall the first
  time — click **Allow**.
- **No "CABLE Output" in Discord's mic list:** VB-CABLE didn't install / needs a
  reboot. Redo step 1.
- **The black window shows "VB-CABLE isn't installed":** same — do step 1.
