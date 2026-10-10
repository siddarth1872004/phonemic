# PhoneMic virtual audio driver (experimental)

A native Windows capture device — "PhoneMic Microphone" — so PhoneMic no longer
needs VB-CABLE. It is a kernel-mode **WDF + ACX** (Audio Class eXtensions) driver
written in C, structured after Microsoft's ACX samples (not forked from any
third-party driver). Audio comes from the desktop app through the shared-memory
ring in [`../ipc`](../ipc).

```
phone ─► PhoneMic app (jitter buffer, Voice Focus, effects)
       ─► ipc/ring.h shared section  Global\PhonemicRing
       ─► this driver's RT capture packets ─► Windows audio stack ─► Discord/Zoom
```

## Status: experimental. Builds in CI, never loaded

CI (job *Windows driver (WDK, experimental)*) compiles and links `phonemic.sys`
with warnings as errors, against the WDK 10.0.26100 headers (ACX 1.0, KMDF 1.33),
and uploads it unsigned as the `PhoneMic-driver-unsigned` artifact. It has
**never been loaded or run** on a real Windows machine. It follows the ACX
RT-stream model (circuit + streaming pin + microphone bridge pin,
`AllocateRtPackets`, `GetCapturePacket`, `AcxRtStreamNotifyPacketComplete`).
Treat it as a careful starting point, not a shipping driver.

### Building without the WDK installer

GitHub's `windows-2022` image has a WDK without ACX. CI fills the gap from the
`Microsoft.Windows.WDK.x64` NuGet package; you can do the same:

```powershell
nuget install Microsoft.Windows.WDK.x64 -Version 10.0.26100.6584 -OutputDirectory $env:TEMP\nuget
msbuild pc-app/driver/phonemic.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off `
  /p:PhonemicSkipInf=true "/p:PhonemicAcxRoot=$env:TEMP\nuget\Microsoft.Windows.WDK.x64.10.0.26100.6584\c"
```

`PhonemicSkipInf=true` skips INF validation and packaging (the runner image's
InfVerif is broken); drop it for an installable package.

Safety properties the code is designed around (and that a reviewer should check):

- The ring is mapped **read/write into system space** and its pages are **locked**
  before the DISPATCH_LEVEL pump touches them.
- The mapped view size is checked against `sizeof(PHONEMIC_RING)`; the ABI header
  is validated; every slot index is taken modulo the capacity, and user-mode
  indices are clamped — a hostile or buggy producer can't make the driver read
  out of bounds.
- If the desktop app isn't running, the stream plays silence and retries the
  mapping once a second from a PASSIVE_LEVEL work item.
- The pump follows an absolute schedule derived from the packet count, so it
  doesn't drift.

## Build

1. Visual Studio 2022 with "Desktop development with C++", the matching Windows
   SDK, the **WDK** and its Visual Studio extension.
2. Open `phonemic.vcxproj` (or create *Kernel Mode Driver, Empty (KMDF)* and add
   `driver.c`, `driver.h`, `phonemic.inf`). ACX requires Windows 11.
3. Enable test-signing on the test machine (admin), then reboot:
   `bcdedit /set testsigning on` (Secure Boot may need to be off).
4. Install: `pnputil /add-driver phonemic.inf /install`, then create the root
   device, e.g. `devgen /add /hardwareid Root\PhonemicCapture` (WDK tool).

## Run

```
phonemic-receiver --driver          (as administrator: it creates Global\PhonemicRing)
```

Then pick **PhoneMic Microphone (virtual)** as the input in your apps.

## Distribution

Test-signing is development-only. Shipping requires attestation signing through
the Windows Hardware Dev Center (or WHQL). Until then the supported path is
VB-CABLE, which the desktop app installs for you.
