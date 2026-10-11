/*++
PhoneMic virtual audio capture driver (WDF + ACX).

See driver.h for STATUS (experimental: builds in CI, never loaded). Flow:

    DriverEntry ─ AcxDriverInitialize
      └─ PhonemicEvtDeviceAdd ─ AcxDeviceInitInitialize / AcxDeviceInitialize
           └─ PhonemicCreateCaptureCircuit: capture circuit, streaming pin +
              microphone bridge pin, 48 kHz / mono / 16-bit format
                └─ PhonemicEvtCircuitCreateStream (per client): RT stream
                     ├─ AllocateRtPackets: non-paged packet buffers
                     ├─ Run: start the pump
                     └─ PumpTimer: ring -> next packet, NotifyPacketComplete

The shared ring (Global\PhonemicRing) is created by the device (DeviceAdd), not
by the app: creating Global\ objects needs administrator rights, opening one
the DACL allows does not. So the desktop app runs unelevated.

Fixes relative to the first draft (which also used non-existent ACX calls):
 - the ring was mapped READ-ONLY and then written (read_index): bugcheck;
 - it was mapped with ZwMapViewOfSection into whatever process was current,
   instead of system space;
 - the pump read pageable section memory at DISPATCH_LEVEL (now locked);
 - the mapped view's size was never checked against sizeof(PHONEMIC_RING);
 - a one-shot timer re-armed with a relative 10 ms drifted; the pump now
   follows an absolute schedule derived from the packet count;
 - Run failed outright if the desktop app wasn't running yet; streams now play
   silence until the app writes into the ring.
--*/

#include <initguid.h>
#include "driver.h"

DECLARE_CONST_UNICODE_STRING(kCircuitName, L"PhoneMic Microphone");

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, PhonemicEvtDeviceAdd)
#pragma alloc_text(PAGE, PhonemicCreateCaptureCircuit)
#pragma alloc_text(PAGE, PhonemicEvtCircuitCreateStream)
#pragma alloc_text(PAGE, PhonemicCreateRing)
#pragma alloc_text(PAGE, PhonemicDestroyRing)
#pragma alloc_text(PAGE, PhonemicEvtDeviceCleanup)
#endif

// ---------------------------------------------------------------------------
// Driver / device
// ---------------------------------------------------------------------------

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    ACX_DRIVER_CONFIG acxConfig;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, PhonemicEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ACX_DRIVER_CONFIG_INIT(&acxConfig);
    return AcxDriverInitialize(driver, &acxConfig);
}

NTSTATUS
PhonemicEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_DEVICEINIT_CONFIG devInitCfg;
    ACX_DEVICE_CONFIG devCfg;
    ACXCIRCUIT circuit;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    ACX_DEVICEINIT_CONFIG_INIT(&devInitCfg);
    status = AcxDeviceInitInitialize(DeviceInit, &devInitCfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PHONEMIC_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = PhonemicEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Without the ring the microphone still works, it just stays silent; the
    // app reports "driver not running" because it can't open the ring.
    (VOID)PhonemicCreateRing(PhonemicDeviceGetContext(device));

    ACX_DEVICE_CONFIG_INIT(&devCfg);
    status = AcxDeviceInitialize(device, &devCfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = PhonemicCreateCaptureCircuit(device, &circuit);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    PhonemicDeviceGetContext(device)->Circuit = circuit;

    // A root-enumerated software device has no hardware to wait for: publish
    // the circuit (Windows then shows the endpoint) right away.
    return AcxDeviceAddCircuit(device, circuit);
}

// The circuit Windows shows as "PhoneMic Microphone": a streaming (source) pin
// that clients read from, and a bridge pin typed as a microphone.
NTSTATUS
PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT* Circuit)
{
    NTSTATUS status;
    PACXCIRCUIT_INIT circuitInit;
    ACXCIRCUIT circuit;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_PIN_CONFIG pinCfg;
    ACXPIN pins[2];
    ACX_JACK_CONFIG jackCfg;
    ACXJACK jack;
    GUID categories[3];
    ACX_DATAFORMAT_CONFIG formatCfg;
    ACXDATAFORMAT format;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE ksFormat;

    PAGED_CODE();
    *Circuit = NULL;

    circuitInit = AcxCircuitInitAllocate(Device);
    if (circuitInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcxCircuitInitSetComponentId(circuitInit, &PHONEMIC_CIRCUIT_COMPONENT_GUID);
    status = AcxCircuitInitAssignName(circuitInit, &kCircuitName);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(circuitInit);
        return status;
    }
    AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeCapture);
    // The categories the audio endpoint builder looks for: without AUDIO +
    // CAPTURE (+ REALTIME for an RT stream) Windows creates no capture endpoint
    // and apps never see the microphone.
    categories[0] = KSCATEGORY_AUDIO;
    categories[1] = KSCATEGORY_CAPTURE;
    categories[2] = KSCATEGORY_REALTIME;
    status = AcxCircuitInitAssignCategories(circuitInit, categories, ARRAYSIZE(categories));
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(circuitInit);
        return status;
    }
    status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit, PhonemicEvtCircuitCreateStream);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(circuitInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = AcxCircuitCreate(Device, &attributes, &circuitInit, &circuit);
    if (!NT_SUCCESS(status)) {
        return status;  // circuitInit is freed by AcxCircuitCreate on failure
    }

    // Streaming pin (index 0): what capture clients connect to.
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSource;
    pinCfg.Communication = AcxPinCommunicationSink;
    pinCfg.Category = &KSCATEGORY_AUDIO;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinCfg, &pins[0]);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Bridge pin (index 1): the physical-side "microphone".
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSink;
    pinCfg.Communication = AcxPinCommunicationNone;
    pinCfg.Category = &KSNODETYPE_MICROPHONE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinCfg, &pins[1]);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // A jack that is always plugged in (no presence detection): Windows hides
    // endpoints whose jack reads "unplugged", so it must never look unplugged.
    ACX_JACK_CONFIG_INIT(&jackCfg);
    jackCfg.Description.ChannelMapping = KSAUDIO_SPEAKER_MONO;
    jackCfg.Description.Color = 0;
    jackCfg.Description.ConnectionType = AcxConnTypeOtherDigital;
    jackCfg.Description.GeoLocation = AcxGeoLocNotApplicable;
    jackCfg.Description.GenLocation = AcxGenLocOther;
    jackCfg.Description.PortConnection = AcxPortConnIntegratedDevice;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = pins[1];
    status = AcxJackCreate(pins[1], &attributes, &jackCfg, &jack);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = AcxPinAddJacks(pins[1], &jack, 1);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // The one format we produce: 48 kHz, 16-bit, mono PCM.
    RtlZeroMemory(&ksFormat, sizeof(ksFormat));
    ksFormat.DataFormat.FormatSize = sizeof(ksFormat);
    ksFormat.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    ksFormat.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    ksFormat.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    ksFormat.DataFormat.SampleSize = PHONEMIC_BLOCK_ALIGN;
    ksFormat.WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ksFormat.WaveFormatExt.Format.nChannels = PHONEMIC_CHANNELS;
    ksFormat.WaveFormatExt.Format.nSamplesPerSec = PHONEMIC_SAMPLE_RATE;
    ksFormat.WaveFormatExt.Format.nAvgBytesPerSec = PHONEMIC_SAMPLE_RATE * PHONEMIC_BLOCK_ALIGN;
    ksFormat.WaveFormatExt.Format.nBlockAlign = PHONEMIC_BLOCK_ALIGN;
    ksFormat.WaveFormatExt.Format.wBitsPerSample = PHONEMIC_BITS_PER_SAMPLE;
    ksFormat.WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ksFormat.WaveFormatExt.Samples.wValidBitsPerSample = PHONEMIC_BITS_PER_SAMPLE;
    ksFormat.WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_MONO;
    ksFormat.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    ACX_DATAFORMAT_CONFIG_INIT_KS(&formatCfg, &ksFormat);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxDataFormatCreate(Device, &attributes, &formatCfg, &format);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = AcxDataFormatListAddDataFormat(AcxPinGetRawDataFormatList(pins[0]), format);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AcxCircuitAddPins(circuit, pins, ARRAYSIZE(pins));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Circuit = circuit;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

NTSTATUS
PhonemicEvtCircuitCreateStream(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ ACXPIN Pin,
    _In_ PACXSTREAM_INIT StreamInit,
    _In_ ACXDATAFORMAT StreamFormat,
    _In_ const GUID* SignalProcessingMode,
    _In_ ACXOBJECTBAG VarArguments)
{
    NTSTATUS status;
    ACXSTREAM stream;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_STREAM_CALLBACKS streamCallbacks;
    ACX_RT_STREAM_CALLBACKS rtCallbacks;
    WDF_TIMER_CONFIG timerCfg;
    PPHONEMIC_STREAM_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(StreamFormat);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);
    PAGED_CODE();

    ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
    streamCallbacks.EvtAcxStreamPrepareHardware = PhonemicEvtStreamPrepare;
    streamCallbacks.EvtAcxStreamReleaseHardware = PhonemicEvtStreamRelease;
    streamCallbacks.EvtAcxStreamRun = PhonemicEvtStreamRun;
    streamCallbacks.EvtAcxStreamPause = PhonemicEvtStreamPause;
    status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &streamCallbacks);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
    rtCallbacks.EvtAcxStreamGetHwLatency = PhonemicEvtStreamGetHwLatency;
    rtCallbacks.EvtAcxStreamAllocateRtPackets = PhonemicEvtStreamAllocateRtPackets;
    rtCallbacks.EvtAcxStreamFreeRtPackets = PhonemicEvtStreamFreeRtPackets;
    rtCallbacks.EvtAcxStreamGetCapturePacket = PhonemicEvtStreamGetCapturePacket;
    rtCallbacks.EvtAcxStreamGetPresentationPosition = PhonemicEvtStreamGetPresentationPosition;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rtCallbacks);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PHONEMIC_STREAM_CONTEXT);
    attributes.EvtCleanupCallback = PhonemicEvtStreamCleanup;
    status = AcxRtStreamCreate(Device, Circuit, &attributes, &StreamInit, &stream);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = PhonemicStreamGetContext(stream);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Ring = PhonemicDeviceGetContext(Device)->Ring;  // outlives the stream (child of the device)

    // High-resolution one-shot timer, re-armed on an absolute schedule.
    WDF_TIMER_CONFIG_INIT(&timerCfg, PhonemicEvtPumpTimer);
    timerCfg.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = stream;
    return WdfTimerCreate(&timerCfg, &attributes, &ctx->PumpTimer);
}

VOID
PhonemicEvtStreamCleanup(_In_ WDFOBJECT Object)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext((ACXSTREAM)Object);
    if (ctx->PumpTimer) {
        WdfTimerStop(ctx->PumpTimer, TRUE);
    }
}

NTSTATUS
PhonemicEvtStreamPrepare(_In_ ACXSTREAM Stream)
{
    UNREFERENCED_PARAMETER(Stream);
    return STATUS_SUCCESS;  // packets are allocated in AllocateRtPackets
}

NTSTATUS
PhonemicEvtStreamRelease(_In_ ACXSTREAM Stream)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    ctx->Running = FALSE;
    WdfTimerStop(ctx->PumpTimer, TRUE);
    return STATUS_SUCCESS;
}

NTSTATUS
PhonemicEvtStreamGetHwLatency(_In_ ACXSTREAM Stream, _Out_ ULONG* FifoSize, _Out_ ULONG* Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    *FifoSize = 0;
    *Delay = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
PhonemicEvtStreamAllocateRtPackets(
    _In_ ACXSTREAM Stream,
    _In_ ULONG PacketCount,
    _In_ ULONG PacketSize,
    _Out_ PACX_RTPACKET* Packets)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    PACX_RTPACKET packets;
    PHYSICAL_ADDRESS low, high, skip;
    ULONG i;

    *Packets = NULL;
    if (PacketCount == 0 || PacketCount > PHONEMIC_MAX_PACKETS || PacketSize == 0 ||
        PacketSize % PHONEMIC_BLOCK_ALIGN != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    packets = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * PacketCount,
                                             PHONEMIC_POOL_TAG);
    if (packets == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    low.QuadPart = 0;
    high.QuadPart = -1;
    skip.QuadPart = 0;
    for (i = 0; i < PacketCount; ++i) {
        PMDL mdl = MmAllocatePagesForMdlEx(low, high, skip, PacketSize, MmCached, MM_ALLOCATE_FULLY_REQUIRED);
        if (mdl == NULL) {
            goto fail;
        }
        ctx->PacketMdl[i] = mdl;
        ctx->PacketVa[i] = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
        if (ctx->PacketVa[i] == NULL) {
            goto fail;
        }
        RtlZeroMemory(ctx->PacketVa[i], PacketSize);
        ACX_RTPACKET_INIT(&packets[i]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[i].RtPacketBuffer, mdl, PacketSize);
        packets[i].RtPacketOffset = 0;
        packets[i].RtPacketSize = PacketSize;
    }
    ctx->PacketCount = PacketCount;
    ctx->PacketSize = PacketSize;
    ctx->SamplesPerPacket = PacketSize / PHONEMIC_BLOCK_ALIGN;
    *Packets = packets;
    return STATUS_SUCCESS;

fail:
    for (i = 0; i < PacketCount; ++i) {
        if (ctx->PacketMdl[i]) {
            MmFreePagesFromMdl(ctx->PacketMdl[i]);
            ExFreePool(ctx->PacketMdl[i]);
            ctx->PacketMdl[i] = NULL;
            ctx->PacketVa[i] = NULL;
        }
    }
    ExFreePoolWithTag(packets, PHONEMIC_POOL_TAG);
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
PhonemicEvtStreamFreeRtPackets(_In_ ACXSTREAM Stream, _In_ PACX_RTPACKET Packets, _In_ ULONG PacketCount)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    ULONG i;
    for (i = 0; i < PacketCount && i < PHONEMIC_MAX_PACKETS; ++i) {
        if (ctx->PacketMdl[i]) {
            MmFreePagesFromMdl(ctx->PacketMdl[i]);
            ExFreePool(ctx->PacketMdl[i]);  // MDLs from MmAllocatePagesForMdlEx are pool-allocated
            ctx->PacketMdl[i] = NULL;
            ctx->PacketVa[i] = NULL;
        }
    }
    ctx->PacketCount = 0;
    ExFreePoolWithTag(Packets, PHONEMIC_POOL_TAG);
}

// Absolute due time (system time, 100 ns) of packet number `n`.
static LONGLONG
PhonemicDueTime(_In_ PPHONEMIC_STREAM_CONTEXT ctx, _In_ ULONGLONG n)
{
    return (LONGLONG)(ctx->StartTime + (n * ctx->SamplesPerPacket * 10000000ull) / PHONEMIC_SAMPLE_RATE);
}

NTSTATUS
PhonemicEvtStreamRun(_In_ ACXSTREAM Stream)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    LARGE_INTEGER now;

    if (ctx->PacketCount == 0 || ctx->SamplesPerPacket == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    // Start at the producer's current position (no stale audio). If the app
    // isn't running, the ring is empty and the stream plays silence.
    if (ctx->Ring != NULL) {
        ctx->RingReadIndex = (ULONGLONG)ReadNoFence64((volatile LONG64*)&ctx->Ring->write_index);
    }

    KeQuerySystemTimePrecise(&now);
    ctx->StartTime = (ULONGLONG)now.QuadPart;
    InterlockedExchange64(&ctx->PacketsCompleted, 0);
    ctx->Running = TRUE;
    WdfTimerStart(ctx->PumpTimer, PhonemicDueTime(ctx, 1));  // positive = absolute
    return STATUS_SUCCESS;
}

NTSTATUS
PhonemicEvtStreamPause(_In_ ACXSTREAM Stream)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    ctx->Running = FALSE;
    WdfTimerStop(ctx->PumpTimer, TRUE);
    return STATUS_SUCCESS;
}

// One capture packet per tick: copy from the ring (or silence), tell the audio
// stack the packet is complete, re-arm for the next packet's absolute time.
VOID
PhonemicEvtPumpTimer(_In_ WDFTIMER Timer)
{
    ACXSTREAM stream = (ACXSTREAM)WdfTimerGetParentObject(Timer);
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(stream);
    ULONGLONG packet;
    INT16* dst;
    ULONG n, i;
    LARGE_INTEGER qpc;

    if (!ctx->Running) {
        return;
    }
    packet = (ULONGLONG)ctx->PacketsCompleted;
    dst = (INT16*)ctx->PacketVa[packet % ctx->PacketCount];
    n = ctx->SamplesPerPacket;

    if (ctx->Ring != NULL) {
        // Acquire the producer's progress before reading samples.
        ULONGLONG write = (ULONGLONG)ReadNoFence64((volatile LONG64*)&ctx->Ring->write_index);
        ULONGLONG avail;
        KeMemoryBarrier();
        avail = write - ctx->RingReadIndex;
        // Never trust user-mode indices: a bogus or far-ahead producer is
        // clamped (slot indices below are always taken modulo the capacity).
        if (avail > PHONEMIC_RING_CAPACITY) {
            ctx->RingReadIndex = write - n;
            avail = n;
        } else if (avail > (ULONGLONG)n * 8) {
            // Far behind (e.g. the client started late): drop to ~2 packets of
            // latency instead of lagging for seconds.
            ctx->RingReadIndex = write - (ULONGLONG)n * 2;
            avail = (ULONGLONG)n * 2;
        }
        for (i = 0; i < n; ++i) {
            dst[i] = (i < avail) ? ctx->Ring->samples[(ctx->RingReadIndex + i) % PHONEMIC_RING_CAPACITY] : 0;
        }
        ctx->RingReadIndex += (avail < n) ? avail : n;
        KeMemoryBarrier();
        WriteNoFence64((volatile LONG64*)&ctx->Ring->read_index, (LONG64)ctx->RingReadIndex);
    } else {
        RtlZeroMemory(dst, ctx->PacketSize);
    }

    qpc = KeQueryPerformanceCounter(NULL);
    InterlockedExchange64(&ctx->LastPacketQpc, qpc.QuadPart);
    InterlockedExchange64(&ctx->PacketsCompleted, (LONG64)(packet + 1));
    AcxRtStreamNotifyPacketComplete(stream, packet, (ULONGLONG)qpc.QuadPart);

    if (ctx->Running) {
        WdfTimerStart(ctx->PumpTimer, PhonemicDueTime(ctx, packet + 2));
    }
}

NTSTATUS
PhonemicEvtStreamGetCapturePacket(
    _In_ ACXSTREAM Stream,
    _Out_ ULONG* LastCapturePacket,
    _Out_ ULONGLONG* QPCPacketStart,
    _Out_ BOOLEAN* MoreData)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    LONG64 done = InterlockedCompareExchange64(&ctx->PacketsCompleted, 0, 0);
    if (done == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    *LastCapturePacket = (ULONG)(done - 1);
    *QPCPacketStart = (ULONGLONG)InterlockedCompareExchange64(&ctx->LastPacketQpc, 0, 0);
    *MoreData = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
PhonemicEvtStreamGetPresentationPosition(
    _In_ ACXSTREAM Stream,
    _Out_ PULONGLONG PositionInBlocks,
    _Out_ PULONGLONG QPCPosition)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    LONG64 done = InterlockedCompareExchange64(&ctx->PacketsCompleted, 0, 0);
    *PositionInBlocks = (ULONGLONG)done * ctx->SamplesPerPacket;
    *QPCPosition = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shared ring (PASSIVE_LEVEL)
// ---------------------------------------------------------------------------

// Creates \BaseNamedObjects\PhonemicRing (Win32: Global\PhonemicRing), maps it
// into system space and locks it. Access: SYSTEM and Administrators full
// control; signed-in users may map it read/write (the desktop app writes the
// audio), but not change its permissions or delete it. Everything user mode can
// write is treated as hostile by the pump: indices are clamped and every slot
// is taken modulo the capacity.
NTSTATUS
PhonemicCreateRing(_Inout_ PPHONEMIC_DEVICE_CONTEXT Dev)
{
    NTSTATUS status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    SECURITY_DESCRIPTOR sd;
    PACL acl;
    ULONG aclSize;
    LARGE_INTEGER maxSize;
    SIZE_T viewSize = 0;
    PHONEMIC_RING* ring;

    PAGED_CODE();

    aclSize = (ULONG)sizeof(ACL) + 3 * (ULONG)FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) +
              RtlLengthSid(SeExports->SeLocalSystemSid) + RtlLengthSid(SeExports->SeAliasAdminsSid) +
              RtlLengthSid(SeExports->SeAuthenticatedUsersSid);
    acl = (PACL)ExAllocatePool2(POOL_FLAG_PAGED, aclSize, PHONEMIC_POOL_TAG);
    if (acl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = RtlCreateAcl(acl, aclSize, ACL_REVISION);
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, SeExports->SeLocalSystemSid);
    }
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, SeExports->SeAliasAdminsSid);
    }
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE,
                                        SeExports->SeAuthenticatedUsersSid);
    }
    if (NT_SUCCESS(status)) {
        status = RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    }
    if (NT_SUCCESS(status)) {
        status = RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE);
    }
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(acl, PHONEMIC_POOL_TAG);
        return status;
    }

    RtlInitUnicodeString(&name, PHONEMIC_RING_SECTION_NAME);
    // OPENIF: if the device restarts while the app still holds the previous
    // section open, the name still exists; reuse it rather than fail.
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NULL, &sd);
    maxSize.QuadPart = sizeof(PHONEMIC_RING);
    status = ZwCreateSection(&Dev->SectionHandle, SECTION_ALL_ACCESS, &oa, &maxSize, PAGE_READWRITE, SEC_COMMIT,
                             NULL);
    ExFreePoolWithTag(acl, PHONEMIC_POOL_TAG);  // the section keeps its own copy
    if (!NT_SUCCESS(status)) {
        Dev->SectionHandle = NULL;
        return status;
    }

    status = ObReferenceObjectByHandle(Dev->SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, NULL, KernelMode,
                                       &Dev->SectionObject, NULL);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    // System space: valid in every process context.
    status = MmMapViewInSystemSpace(Dev->SectionObject, &Dev->RingView, &viewSize);
    if (!NT_SUCCESS(status)) {
        Dev->RingView = NULL;
        goto fail;
    }
    if (viewSize < sizeof(PHONEMIC_RING)) {
        status = STATUS_INVALID_BUFFER_SIZE;  // a pre-existing section of another size
        goto fail;
    }
    // Lock the pages so the DISPATCH_LEVEL pump can never page-fault.
    Dev->RingMdl = IoAllocateMdl(Dev->RingView, sizeof(PHONEMIC_RING), FALSE, FALSE, NULL);
    if (Dev->RingMdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    __try {
        MmProbeAndLockPages(Dev->RingMdl, KernelMode, IoModifyAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(Dev->RingMdl);
        Dev->RingMdl = NULL;
        status = STATUS_ACCESS_VIOLATION;
        goto fail;
    }

    // Publish the layout the app checks before writing. The indices are left
    // alone: a reused section keeps the app's write position.
    ring = (PHONEMIC_RING*)Dev->RingView;
    ring->capacity = PHONEMIC_RING_CAPACITY;
    ring->sample_rate = PHONEMIC_SAMPLE_RATE;
    ring->channels = PHONEMIC_CHANNELS;
    KeMemoryBarrier();
    ring->abi_version = PHONEMIC_RING_ABI_VERSION;
    Dev->Ring = ring;
    return STATUS_SUCCESS;

fail:
    PhonemicDestroyRing(Dev);
    return status;
}

VOID
PhonemicDestroyRing(_Inout_ PPHONEMIC_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();
    Dev->Ring = NULL;
    if (Dev->RingMdl) {
        MmUnlockPages(Dev->RingMdl);
        IoFreeMdl(Dev->RingMdl);
        Dev->RingMdl = NULL;
    }
    if (Dev->RingView) {
        MmUnmapViewInSystemSpace(Dev->RingView);
        Dev->RingView = NULL;
    }
    if (Dev->SectionObject) {
        ObDereferenceObject(Dev->SectionObject);
        Dev->SectionObject = NULL;
    }
    if (Dev->SectionHandle) {
        ZwClose(Dev->SectionHandle);
        Dev->SectionHandle = NULL;
    }
}

// Streams are children of the device, so they are gone (pumps stopped) before
// this runs.
VOID
PhonemicEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PAGED_CODE();
    PhonemicDestroyRing(PhonemicDeviceGetContext((WDFDEVICE)Object));
}
