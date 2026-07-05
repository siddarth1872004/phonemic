/*++
PhoneMic virtual audio capture driver — shared declarations.

Structured after Microsoft's ACX `sysvad` sample: a WDF driver that publishes an
ACX circuit exposing a single **capture** endpoint. The capture stream is fed
from the shared-memory ring (../ipc/ring.h) that the user-mode core fills with
PCM received from the phone.

STATUS: source is code-complete against the ACX model but has NOT been compiled
— it requires the WDK (see BUILDING notes in README.md) which is not installed
in the development environment. Treat signatures as reviewed-but-unverified until
built against a real WDK.
--*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acx.h>

#include "..\ipc\ring.h"

// Stable component identity for the capture circuit. Regenerate with guidgen if
// forking; keep it constant across releases so the endpoint identity is stable.
// {8F3A1C6E-2B4D-4E9A-9C1F-7A2E5D6B0011}
DEFINE_GUID(PHONEMIC_CIRCUIT_COMPONENT_GUID,
    0x8f3a1c6e, 0x2b4d, 0x4e9a, 0x9c, 0x1f, 0x7a, 0x2e, 0x5d, 0x6b, 0x00, 0x11);

// Format we expose: 48 kHz, mono, 16-bit PCM — matching the wire format so no
// resampling happens anywhere in the pipeline.
#define PHONEMIC_SAMPLE_RATE   48000
#define PHONEMIC_CHANNELS      1
#define PHONEMIC_BITS_PER_SAMPLE 16

// Per-device context.
typedef struct _PHONEMIC_DEVICE_CONTEXT {
    ACXCIRCUIT  Circuit;        // the capture circuit we publish
    PHONEMIC_RING* Ring;        // mapped shared ring (producer = user mode)
    PMDL        RingMdl;        // MDL describing the ring section
    HANDLE      RingSection;    // section handle from the establishing IOCTL
} PHONEMIC_DEVICE_CONTEXT, *PPHONEMIC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_DEVICE_CONTEXT, PhonemicDeviceGetContext)

// Per-stream context.
typedef struct _PHONEMIC_STREAM_CONTEXT {
    PPHONEMIC_DEVICE_CONTEXT Device;
    BOOLEAN Running;
} PHONEMIC_STREAM_CONTEXT, *PPHONEMIC_STREAM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_STREAM_CONTEXT, PhonemicStreamGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PhonemicEvtDeviceAdd;
EVT_ACX_CIRCUIT_CREATE_STREAM PhonemicEvtCircuitCreateStream;

NTSTATUS PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device);
