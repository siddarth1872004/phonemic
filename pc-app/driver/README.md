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

## Own build system

This subdirectory has its own MSBuild/WDK build, separate from the Cargo
workspace. It is intentionally not a Cargo member.
