# PhoneMic virtual microphone driver for Windows

PhoneMic's own capture device, "PhoneMic Microphone", so no third-party virtual
cable is needed. A kernel-mode **WDF + ACX** (Audio Class eXtensions) driver
written in C, structured after Microsoft's ACX samples (not forked from any
third-party driver). Windows 11 22H2 or newer.

```
phone ─► PhoneMic app (jitter buffer, Voice Focus, effects, adaptive playout)
       ─► Global\PhonemicRing (shared memory, created by this driver)
       ─► this driver's RT capture packets ─► Windows audio stack ─► Discord/Zoom
```

The driver creates the ring when its device starts, with a DACL that lets
signed-in users map it, so the app (an ordinary, non-elevated process) opens it
and writes into it. The app paces the ring with the driver's own clock
(`ipc/ring_feeder.*`): it keeps ~20 ms queued and refills exactly what the
driver consumed, so phone/PC clock drift is absorbed by the playout buffer.

## Install (users)

The Windows release zip has a `driver` folder next to `PhoneMic.exe`. In the app,
click **Install PhoneMic driver**, or run `driver\install-driver.ps1`. It:

1. checks for Windows 11 22H2+;
2. explains **test mode** and asks before turning it on (needed until the driver
   is attestation-signed by Microsoft; Secure Boot must be off; a "Test Mode"
   watermark appears; some anti-cheat games refuse to run in it);
3. creates a code-signing certificate that exists only on this PC and trusts it
   for driver installs there;
4. signs a copy of the package (`%ProgramData%\PhoneMic\driver`) with it;
5. creates the root device `Root\PhonemicCapture` and installs the driver
   (`phonemic-driver.exe install`, a small SetupAPI tool, like devcon).

Restart when it says so. Then pick **PhoneMic Microphone** in your apps.
`driver\uninstall-driver.ps1` removes the device, the driver package and the
certificate, and offers to turn test mode off. `phonemic-driver.exe status`
shows whether the device exists and the driver is running.

On Windows 10, or without test mode, the app's **Use VB-CABLE instead** button
sets up VB-CABLE as a fallback.

## Status

CI (job *Windows driver (WDK)*) compiles and links `phonemic.sys` with warnings
as errors against the WDK 10.0.26100 headers (ACX 1.0, KMDF 1.33) and builds the
catalog with Inf2Cat; the package ships inside the Windows zip. The app side
(opening the ring, feeding it at the driver's pace) is unit-tested, including
on Windows against a stand-in ring. The driver itself has **not yet been loaded
on real hardware by the project**: please report how it goes.

Safety properties the code is designed around (and that a reviewer should check):

- The ring is mapped **read/write into system space** and its pages are **locked**
  before the DISPATCH_LEVEL pump touches them.
- The mapped view size is checked against `sizeof(PHONEMIC_RING)` (a pre-existing
  section with the same name is reused only if it is big enough). The driver
  writes the header and never reads layout values back from shared memory; every
  slot index is taken modulo the capacity and user-mode indices are clamped — a
  hostile or buggy producer can't make the driver read out of bounds.
- The ring is created by the driver at PASSIVE_LEVEL when the device starts and
  lives as long as the device. If the app isn't running, streams play silence.
- Users may map the ring read/write but not change its DACL or delete it; only
  SYSTEM and Administrators have full control.
- The pump follows an absolute schedule derived from the packet count, so it
  doesn't drift.

## Build (developers)

1. Visual Studio 2022 with "Desktop development with C++", the matching Windows
   SDK, the **WDK** and its Visual Studio extension; open `phonemic.vcxproj`.
2. Or without the WDK installer, as CI does, using the WDK NuGet package:

```powershell
nuget install Microsoft.Windows.WDK.x64 -Version 10.0.26100.6584 -OutputDirectory $env:TEMP\nuget
msbuild pc-app/driver/phonemic.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off `
  /p:PhonemicSkipInf=true "/p:PhonemicAcxRoot=$env:TEMP\nuget\Microsoft.Windows.WDK.x64.10.0.26100.6584\c"
Inf2Cat /driver:<folder with phonemic.sys + phonemic.inf> /os:10_NI_X64
```

`phonemic.inf` is ready to package as is (no stampinf step): bump `DriverVer`
with each release. Put `phonemic.sys`, `.inf`, `.cat`, the two scripts and
`phonemic-driver.exe` (built by CMake on Windows) in one folder to install.

## Signing for everyone

Test mode is the price of an unsigned driver. Removing it needs Microsoft
**attestation signing**: an EV code-signing certificate and a Windows Hardware
Dev Center (Partner Center) account, then submitting the package (CAB) for
signing. Once signed, `install-driver.ps1` can skip the test-mode and local
certificate steps.
