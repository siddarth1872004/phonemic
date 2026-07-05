# PhoneMic virtual audio driver (Phase 4 — not started)

The end-state Windows component: a real kernel-mode audio driver, written in C
against the WDK's **ACX (Audio Class Extensions)** framework and structured
conceptually after Microsoft's **`sysvad`** sample — *not* forked from any
third-party driver. It exposes a proper **capture endpoint** that appears in
Windows' Sound settings as a microphone, fed by audio the Rust core receives
from the phone.

This is deliberately built last: the whole loop (capture → network → decode →
audible playback) is proven with the `cpal` dev sink first (Phases 0–1).

## Prerequisites (when we get here)

- Windows Driver Kit (WDK) matching the installed Visual Studio.
- Test-signing enabled on the dev machine: `bcdedit /set testsigning on`.
- The IPC ring from `../ipc` as the audio source.

## Signing plan (mentioned, not executed)

For real distribution the driver must be signed. Path: attestation signing via
the Windows Hardware Dev Center dashboard (no EV-signed kernel binary required
for attestation-signed drivers on Win10/11), or a full WHQL submission later.
Until then, development runs under test-signing only. This is a hard requirement
before PhoneMic ships the driver to end users and is called out here so it is
never a surprise.

## Files

- `driver.h` — device/stream contexts, format, component GUID, callback decls.
- `driver.c` — `DriverEntry` → `PhonemicEvtDeviceAdd` → `PhonemicCreateCaptureCircuit`
  → `PhonemicEvtCircuitCreateStream`; the ACX capture circuit whose RT packets
  are pulled from the shared ring.
- `phonemic.inf` — root-enumerated MEDIA-class install (placeholder GUIDs).
- `../ipc/ring.h` — the SPSC shared-memory ring contract the stream reads from.

## Status: code-complete against the ACX model, NOT yet built

The source follows the `sysvad` ACX capture structure and is internally
consistent, but it has **not been compiled** — there is no WDK in the dev
environment, so ACX signatures are reviewed-but-unverified. The remaining work
to a working driver: build against the WDK, wire the establishing IOCTL that
maps the user-mode ring section, and implement the three RT stream callbacks
(`Run`/`Pause`/`GetCapturePacket`) whose bodies copy from `PHONEMIC_RING`.

## Build (when a WDK is available)

Own MSBuild/WDK build, separate from the Cargo workspace (intentionally not a
Cargo member). Needs: WDK matching the installed Visual Studio, test-signing
enabled (`bcdedit /set testsigning on`), then `stampinf` + `signtool` for a test
`.cat`. Install with `pnputil /add-driver phonemic.inf /install`.
