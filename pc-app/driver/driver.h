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

// The user-mode core creates a named section at this path and fills it with
// PCM; the driver maps it read-only on stream start. A well-known name avoids
// handle duplication — the "establishing IOCTL" degenerates to this contract.
#define PHONEMIC_RING_SECTION_NAME L"\\BaseNamedObjects\\PhonemicRing"

// 10 ms of audio per RT packet, and a small ring of packets for the capture RT
// model. Frame size must match the user-mode producer's cadence.
#define PHONEMIC_PACKET_SAMPLES  (PHONEMIC_SAMPLE_RATE / 100)   // 480
#define PHONEMIC_PACKET_BYTES    (PHONEMIC_PACKET_SAMPLES * sizeof(INT16))
#define PHONEMIC_PACKET_COUNT    4

// Per-device context.
typedef struct _PHONEMIC_DEVICE_CONTEXT {
    ACXCIRCUIT  Circuit;        // the capture circuit we publish
} PHONEMIC_DEVICE_CONTEXT, *PPHONEMIC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_DEVICE_CONTEXT, PhonemicDeviceGetContext)

// Per-stream context: owns the ring mapping and the RT packet pump.
typedef struct _PHONEMIC_STREAM_CONTEXT {
    PPHONEMIC_DEVICE_CONTEXT Device;

    // Mapped shared ring (produced by user mode).
    HANDLE          SectionHandle;
    PVOID           RingBase;       // mapped view; cast to PHONEMIC_RING*
    PHONEMIC_RING*  Ring;

    // ACX RT capture packets the audio stack reads from.
    PVOID           Packets[PHONEMIC_PACKET_COUNT];
    ULONG           CurrentPacket;  // index of the last completed packet
    ULONGLONG       RingReadIndex;  // our consumer cursor into Ring->samples

    // Timer that copies one packet's worth of samples every 10 ms.
    WDFTIMER        PumpTimer;
    BOOLEAN         Running;
} PHONEMIC_STREAM_CONTEXT, *PPHONEMIC_STREAM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_STREAM_CONTEXT, PhonemicStreamGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PhonemicEvtDeviceAdd;
EVT_ACX_CIRCUIT_CREATE_STREAM PhonemicEvtCircuitCreateStream;
EVT_ACX_STREAM_PREPARE_HARDWARE PhonemicEvtStreamPrepare;
EVT_ACX_STREAM_RUN PhonemicEvtStreamRun;
EVT_ACX_STREAM_PAUSE PhonemicEvtStreamPause;
EVT_ACX_STREAM_GET_CAPTURE_PACKET PhonemicEvtStreamGetCapturePacket;
EVT_WDF_TIMER PhonemicEvtPumpTimer;

NTSTATUS PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device);
NTSTATUS PhonemicMapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Stream);
VOID     PhonemicUnmapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Stream);
