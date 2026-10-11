/*++
PhoneMic virtual audio capture driver — shared declarations.

A WDF + ACX (Audio Class eXtensions) driver, structured after Microsoft's ACX
samples, that publishes one capture circuit: a streaming pin plus a
"microphone" bridge pin. The capture stream is fed from the shared-memory ring
(../ipc/ring.h). The driver creates that ring when the device starts, with a
DACL that lets signed-in users map it, so the PhoneMic desktop app (an ordinary,
non-elevated process) can open it and write audio into it.

STATUS: experimental. CI compiles and links it against the WDK 10.0.26100
headers (ACX 1.0, KMDF 1.33), but it has never been loaded or run on Windows
(see README.md).
--*/
#pragma once

#include <ntifs.h>  // ntddk.h plus the ACL / SID helpers used to secure the ring
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

// Created by the driver; the desktop app opens it as Global\PhonemicRing.
#define PHONEMIC_RING_SECTION_NAME L"\\BaseNamedObjects\\PhonemicRing"

#define PHONEMIC_MAX_PACKETS 8

typedef struct _PHONEMIC_DEVICE_CONTEXT {
    ACXCIRCUIT Circuit;

    // The shared ring, mapped into SYSTEM space with its pages locked so the
    // DISPATCH_LEVEL pump can read it. Ring is NULL if it couldn't be created
    // (streams then play silence).
    HANDLE          SectionHandle;
    PVOID           SectionObject;
    PVOID           RingView;
    PMDL            RingMdl;
    PHONEMIC_RING*  Ring;
} PHONEMIC_DEVICE_CONTEXT, *PPHONEMIC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PHONEMIC_DEVICE_CONTEXT, PhonemicDeviceGetContext)

// Per-stream state. Fields touched by the DISPATCH_LEVEL pump live in
// non-paged memory (WDF contexts are non-paged).
typedef struct _PHONEMIC_STREAM_CONTEXT {
    PHONEMIC_RING*  Ring;           // the device's ring (owned by the device)
    ULONGLONG       RingReadIndex;  // our consumer cursor

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
EVT_WDF_OBJECT_CONTEXT_CLEANUP PhonemicEvtDeviceCleanup;
EVT_WDF_TIMER PhonemicEvtPumpTimer;

NTSTATUS PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT* Circuit);
NTSTATUS PhonemicCreateRing(_Inout_ PPHONEMIC_DEVICE_CONTEXT Dev);
VOID     PhonemicDestroyRing(_Inout_ PPHONEMIC_DEVICE_CONTEXT Dev);
