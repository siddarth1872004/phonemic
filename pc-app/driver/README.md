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

## Status: experimental, not yet compiled

The project's CI has no Windows Driver Kit, so this code has **never been
built or loaded**. It follows the ACX RT-stream model (circuit + streaming pin +
microphone bridge pin, `AllocateRtPackets`, `GetCapturePacket`,
`AcxRtStreamNotifyPacketComplete`), but expect to adjust details against your
WDK's `acx.h`. Treat it as a careful starting point, not a shipping driver.

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
