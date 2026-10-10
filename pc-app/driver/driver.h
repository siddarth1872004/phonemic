/*++
PhoneMic virtual audio capture driver — shared declarations.

A WDF + ACX (Audio Class eXtensions) driver, structured after Microsoft's ACX
samples, that publishes one capture circuit: a streaming pin plus a
"microphone" bridge pin. The capture stream is fed from the shared-memory ring
(../ipc/ring.h) that the PhoneMic desktop app fills (phonemic-receiver --driver).

STATUS: experimental and NOT yet compiled — there is no WDK in the project's
CI. The structure follows the ACX RT-stream model; expect to adjust details to
your WDK's acx.h when building (see README.md).
--*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acx.h>
// Same include order portcls.h uses for kernel audio: windef + mmreg (for
// WAVEFORMATEXTENSIBLE) before the KS headers.
#include <windef.h>
#define NOBITMAP
#include <mmreg.h>
#undef NOBITMAP
#include <ks.h>
#include <ksmedia.h>

#include "..\ipc\ring.h"

#define PHONEMIC_POOL_TAG 'cMhP'

// Stable component identity for the capture circuit. Keep it constant across
// releases so Windows keeps the endpoint's identity (and the user's settings).
// {8F3A1C6E-2B4D-4E9A-9C1F-7A2E5D6B0011}
DEFINE_GUID(PHONEMIC_CIRCUIT_COMPONENT_GUID,
    0x8f3a1c6e, 0x2b4d, 0x4e9a, 0x9c, 0x1f, 0x7a, 0x2e, 0x5d, 0x6b, 0x00, 0x11);

// Exposed format: 48 kHz, mono, 16-bit PCM — the ring's format, so nothing is
// resampled in the kernel.
#define PHONEMIC_BITS_PER_SAMPLE 16
#define PHONEMIC_BLOCK_ALIGN     (PHONEMIC_CHANNELS * PHONEMIC_BITS_PER_SAMPLE / 8)

// The desktop app creates Global\PhonemicRing; the driver opens it by name.
#define PHONEMIC_RING_SECTION_NAME L"\\BaseNamedObjects\\PhonemicRing"

#define PHONEMIC_MAX_PACKETS 8

typedef struct _PHONEMIC_DEVICE_CONTEXT {
    ACXCIRCUIT Circuit;
} PHONEMIC_DEVICE_CONTEXT, *PPHONEMIC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_DEVICE_CONTEXT, PhonemicDeviceGetContext)

// Per-stream state. Fields touched by the DISPATCH_LEVEL pump live in
// non-paged memory (WDF contexts are non-paged); the shared ring is mapped into
// SYSTEM space and its pages are locked, so the pump can read it at DISPATCH.
typedef struct _PHONEMIC_STREAM_CONTEXT {
    // Shared ring (produced by user mode). NULL until the app is running.
    HANDLE          SectionHandle;
    PVOID           SectionObject;
    PVOID           RingView;       // system-space view
    PMDL            RingMdl;        // locks the view's pages
    PHONEMIC_RING*  Ring;
    ULONGLONG       RingReadIndex;  // our consumer cursor
    LONG            MapPending;     // a map work item is queued
    ULONGLONG       LastMapAttempt; // interrupt time (100 ns) of the last try
    WDFWORKITEM     MapWorkItem;

    // RT packets handed to the audio stack (non-paged, from MmAllocatePagesForMdlEx).
    ULONG           PacketCount;
    ULONG           PacketSize;     // bytes
    ULONG           SamplesPerPacket;
    PMDL            PacketMdl[PHONEMIC_MAX_PACKETS];
    PVOID           PacketVa[PHONEMIC_MAX_PACKETS];

    // Pump: one packet per tick, re-armed on an absolute schedule (no drift).
    WDFTIMER        PumpTimer;
    BOOLEAN         Running;
    ULONGLONG       StartTime;          // system time (100 ns) at Run
    volatile LONG64 PacketsCompleted;   // monotonic packet number
    volatile LONG64 LastPacketQpc;
} PHONEMIC_STREAM_CONTEXT, *PPHONEMIC_STREAM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_STREAM_CONTEXT, PhonemicStreamGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PhonemicEvtDeviceAdd;
EVT_ACX_CIRCUIT_CREATE_STREAM PhonemicEvtCircuitCreateStream;

EVT_ACX_STREAM_PREPARE_HARDWARE PhonemicEvtStreamPrepare;
EVT_ACX_STREAM_RELEASE_HARDWARE PhonemicEvtStreamRelease;
EVT_ACX_STREAM_RUN PhonemicEvtStreamRun;
EVT_ACX_STREAM_PAUSE PhonemicEvtStreamPause;
EVT_ACX_STREAM_GET_HW_LATENCY PhonemicEvtStreamGetHwLatency;
EVT_ACX_STREAM_ALLOCATE_RTPACKETS PhonemicEvtStreamAllocateRtPackets;
EVT_ACX_STREAM_FREE_RTPACKETS PhonemicEvtStreamFreeRtPackets;
EVT_ACX_STREAM_GET_CAPTURE_PACKET PhonemicEvtStreamGetCapturePacket;
EVT_ACX_STREAM_GET_PRESENTATION_POSITION PhonemicEvtStreamGetPresentationPosition;
EVT_WDF_OBJECT_CONTEXT_CLEANUP PhonemicEvtStreamCleanup;
EVT_WDF_TIMER PhonemicEvtPumpTimer;
EVT_WDF_WORKITEM PhonemicEvtMapWorkItem;

NTSTATUS PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT* Circuit);
NTSTATUS PhonemicMapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Ctx);
VOID     PhonemicUnmapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Ctx);
