/*++
PhoneMic virtual audio capture driver (WDF + ACX).

See driver.h for STATUS (experimental, not yet compiled). Flow:

    DriverEntry ─ AcxDriverInitialize
      └─ PhonemicEvtDeviceAdd ─ AcxDeviceInitInitialize / AcxDeviceInitialize
           └─ PhonemicCreateCaptureCircuit: capture circuit, streaming pin +
              microphone bridge pin, 48 kHz / mono / 16-bit format
                └─ PhonemicEvtCircuitCreateStream (per client): RT stream
                     ├─ AllocateRtPackets: non-paged packet buffers
                     ├─ Run: start the pump (maps the ring when the app runs)
                     └─ PumpTimer: ring -> next packet, NotifyPacketComplete

Fixes relative to the first draft (which also used non-existent ACX calls):
 - the ring was mapped READ-ONLY and then written (read_index): bugcheck;
 - it was mapped with ZwMapViewOfSection into whatever process was current,
   instead of system space;
 - the pump read pageable section memory at DISPATCH_LEVEL (now locked);
 - the mapped view's size was never checked against sizeof(PHONEMIC_RING);
 - a one-shot timer re-armed with a relative 10 ms drifted; the pump now
   follows an absolute schedule derived from the packet count;
 - Run failed outright if the desktop app wasn't running yet; it now plays
   silence and keeps retrying the mapping from a PASSIVE work item.
--*/

#include <initguid.h>
#include "driver.h"

DECLARE_CONST_UNICODE_STRING(kCircuitName, L"PhoneMic Microphone");

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, PhonemicEvtDeviceAdd)
#pragma alloc_text(PAGE, PhonemicCreateCaptureCircuit)
#pragma alloc_text(PAGE, PhonemicEvtCircuitCreateStream)
#pragma alloc_text(PAGE, PhonemicMapRing)
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
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

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
    WDF_WORKITEM_CONFIG workCfg;
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

    // High-resolution one-shot timer, re-armed on an absolute schedule.
    WDF_TIMER_CONFIG_INIT(&timerCfg, PhonemicEvtPumpTimer);
    timerCfg.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = stream;
    status = WdfTimerCreate(&timerCfg, &attributes, &ctx->PumpTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Mapping the section must happen at PASSIVE_LEVEL: done by a work item.
    WDF_WORKITEM_CONFIG_INIT(&workCfg, PhonemicEvtMapWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = stream;
    return WdfWorkItemCreate(&workCfg, &attributes, &ctx->MapWorkItem);
}

VOID
PhonemicEvtStreamCleanup(_In_ WDFOBJECT Object)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext((ACXSTREAM)Object);
    if (ctx->PumpTimer) {
        WdfTimerStop(ctx->PumpTimer, TRUE);
    }
    if (ctx->MapWorkItem) {
        WdfWorkItemFlush(ctx->MapWorkItem);
    }
    PhonemicUnmapRing(ctx);
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
    WdfTimerStop(ctx->PumpTimer, TRUE);
    WdfWorkItemFlush(ctx->MapWorkItem);
    PhonemicUnmapRing(ctx);
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
    // If the desktop app isn't running yet we still start: the stream plays
    // silence and the pump keeps retrying the mapping.
    (VOID)PhonemicMapRing(ctx);

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
        // Retry the mapping about once a second (needs PASSIVE_LEVEL).
        if ((ULONGLONG)KeQueryInterruptTime() - ctx->LastMapAttempt > 10000000ull &&
            InterlockedCompareExchange(&ctx->MapPending, 1, 0) == 0) {
            ctx->LastMapAttempt = (ULONGLONG)KeQueryInterruptTime();
            WdfWorkItemEnqueue(ctx->MapWorkItem);
        }
    }

    qpc = KeQueryPerformanceCounter(NULL);
    InterlockedExchange64(&ctx->LastPacketQpc, qpc.QuadPart);
    InterlockedExchange64(&ctx->PacketsCompleted, (LONG64)(packet + 1));
    AcxRtStreamNotifyPacketComplete(stream, packet, (ULONGLONG)qpc.QuadPart);

    if (ctx->Running) {
        WdfTimerStart(ctx->PumpTimer, PhonemicDueTime(ctx, packet + 2));
    }
}

VOID
PhonemicEvtMapWorkItem(_In_ WDFWORKITEM WorkItem)
{
    ACXSTREAM stream = (ACXSTREAM)WdfWorkItemGetParentObject(WorkItem);
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(stream);
    if (ctx->Ring == NULL) {
        (VOID)PhonemicMapRing(ctx);
    }
    InterlockedExchange(&ctx->MapPending, 0);
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
// Shared ring mapping (PASSIVE_LEVEL)
// ---------------------------------------------------------------------------

NTSTATUS
PhonemicMapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Ctx)
{
    NTSTATUS status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    SIZE_T viewSize = 0;
    PHONEMIC_RING* ring;

    PAGED_CODE();
    if (Ctx->Ring != NULL) {
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&name, PHONEMIC_RING_SECTION_NAME);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    // Read AND write: the consumer publishes read_index back to the producer.
    status = ZwOpenSection(&Ctx->SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, &oa);
    if (!NT_SUCCESS(status)) {
        Ctx->SectionHandle = NULL;
        return status;  // the desktop app isn't running (yet)
    }
    status = ObReferenceObjectByHandle(Ctx->SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, NULL,
                                       KernelMode, &Ctx->SectionObject, NULL);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    // System space: valid in every process context, unlike a view mapped into
    // whichever process happened to be current.
    status = MmMapViewInSystemSpace(Ctx->SectionObject, &Ctx->RingView, &viewSize);
    if (!NT_SUCCESS(status)) {
        Ctx->RingView = NULL;
        goto fail;
    }
    if (viewSize < sizeof(PHONEMIC_RING)) {
        status = STATUS_INVALID_BUFFER_SIZE;  // someone else's section with our name
        goto fail;
    }
    // Lock the pages so the DISPATCH_LEVEL pump can never page-fault.
    Ctx->RingMdl = IoAllocateMdl(Ctx->RingView, sizeof(PHONEMIC_RING), FALSE, FALSE, NULL);
    if (Ctx->RingMdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    __try {
        MmProbeAndLockPages(Ctx->RingMdl, KernelMode, IoModifyAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(Ctx->RingMdl);
        Ctx->RingMdl = NULL;
        status = STATUS_ACCESS_VIOLATION;
        goto fail;
    }

    ring = (PHONEMIC_RING*)Ctx->RingView;
    if (ring->abi_version != PHONEMIC_RING_ABI_VERSION || ring->capacity != PHONEMIC_RING_CAPACITY ||
        ring->sample_rate != PHONEMIC_SAMPLE_RATE || ring->channels != PHONEMIC_CHANNELS) {
        status = STATUS_REVISION_MISMATCH;
        goto fail;
    }
    // Start at the producer's current position (no stale audio).
    Ctx->RingReadIndex = (ULONGLONG)ReadNoFence64((volatile LONG64*)&ring->write_index);
    KeMemoryBarrier();
    Ctx->Ring = ring;  // publish last: the pump checks this
    return STATUS_SUCCESS;

fail:
    PhonemicUnmapRing(Ctx);
    return status;
}

VOID
PhonemicUnmapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Ctx)
{
    Ctx->Ring = NULL;
    KeMemoryBarrier();
    if (Ctx->RingMdl) {
        MmUnlockPages(Ctx->RingMdl);
        IoFreeMdl(Ctx->RingMdl);
        Ctx->RingMdl = NULL;
    }
    if (Ctx->RingView) {
        MmUnmapViewInSystemSpace(Ctx->RingView);
        Ctx->RingView = NULL;
    }
    if (Ctx->SectionObject) {
        ObDereferenceObject(Ctx->SectionObject);
        Ctx->SectionObject = NULL;
    }
    if (Ctx->SectionHandle) {
        ZwClose(Ctx->SectionHandle);
        Ctx->SectionHandle = NULL;
    }
}
