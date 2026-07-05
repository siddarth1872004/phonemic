/*++
PhoneMic virtual audio capture driver — entry, device add, and capture circuit.

See driver.h for STATUS (code-complete, needs WDK to build). The flow mirrors the
ACX `sysvad` capture path:

    DriverEntry
      └─ PhonemicEvtDeviceAdd (per device)
           └─ PhonemicCreateCaptureCircuit
                └─ ACX circuit with one capture endpoint
                     └─ PhonemicEvtCircuitCreateStream (per client stream)
                          └─ RT packets pulled from the shared ring
--*/

#include "driver.h"

#pragma code_seg("INIT")
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, PhonemicEvtDeviceAdd);

    // ACX must see the driver config before WdfDriverCreate so it can hook the
    // class extension.
    AcxDriverConfigInit(&config);

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return STATUS_SUCCESS;
}
#pragma code_seg()

NTSTATUS
PhonemicEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    UNREFERENCED_PARAMETER(Driver);

    // Let ACX augment the device init (adds the audio class plumbing).
    status = AcxDeviceInitInitialize(DeviceInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PHONEMIC_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Register the device with ACX (default settings are fine for a virtual
    // device with no hardware resources).
    ACX_DEVICE_INIT_CONFIG devCfg;
    ACX_DEVICE_INIT_CONFIG_INIT(&devCfg);
    status = AcxDeviceInitialize(device, &devCfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Map the shared ring the user-mode core produces into. In the real build
    // this happens on the establishing IOCTL (open a named section, map it,
    // validate abi_version) — stubbed here as the one piece of glue that needs
    // the user-mode handshake wired up.
    // status = PhonemicMapRing(PhonemicDeviceGetContext(device));

    return PhonemicCreateCaptureCircuit(device);
}

// Build the ACX circuit that shows up as a microphone in Windows.
NTSTATUS
PhonemicCreateCaptureCircuit(_In_ WDFDEVICE Device)
{
    NTSTATUS status;
    PACXCIRCUIT_INIT circuitInit = NULL;
    ACXCIRCUIT circuit;
    WDF_OBJECT_ATTRIBUTES attributes;

    circuitInit = AcxCircuitInitAllocate(Device);
    if (circuitInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Identify as a capture (recording) circuit.
    AcxCircuitInitSetComponentId(circuitInit, &PHONEMIC_CIRCUIT_COMPONENT_GUID);
    AcxCircuitInitAssignName(circuitInit, L"PhoneMic Capture");

    // One stream-create callback services every client that opens the mic.
    ACX_CIRCUIT_INIT_CALLBACKS callbacks;
    ACX_CIRCUIT_INIT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtAcxCircuitCreateStream = PhonemicEvtCircuitCreateStream;
    AcxCircuitInitSetAcxCircuitInitCallbacks(circuitInit, &callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = AcxCircuitCreate(Device, &attributes, &circuitInit, &circuit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PhonemicDeviceGetContext(Device)->Circuit = circuit;

    // Publish the circuit so the audio stack enumerates the endpoint.
    return AcxDeviceAddCircuit(Device, circuit);
}

// Per-client capture stream. Each pull of an RT packet copies the next span of
// samples out of the shared ring; if the producer is behind we emit silence so
// the capture timeline never stalls (the audio stack requires continuous data).
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
    UNREFERENCED_PARAMETER(Circuit);
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(StreamFormat);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PHONEMIC_STREAM_CONTEXT);

    // Wire the RT stream callbacks. Prepare allocates the RT packets, Run maps
    // the shared ring and starts the pump timer, Pause stops it, and
    // GetCapturePacket reports which packet the audio stack may read.
    ACX_RT_STREAM_CALLBACKS rt;
    ACX_RT_STREAM_CALLBACKS_INIT(&rt);
    rt.EvtAcxStreamPrepareHardware  = PhonemicEvtStreamPrepare;
    rt.EvtAcxStreamRun              = PhonemicEvtStreamRun;
    rt.EvtAcxStreamPause            = PhonemicEvtStreamPause;
    rt.EvtAcxStreamGetCapturePacket = PhonemicEvtStreamGetCapturePacket;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rt);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AcxStreamCreate(Device, StreamInit, &attributes, &stream);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PPHONEMIC_STREAM_CONTEXT streamCtx = PhonemicStreamGetContext(stream);
    streamCtx->Device = PhonemicDeviceGetContext(Device);
    streamCtx->CurrentPacket = 0;
    streamCtx->RingReadIndex = 0;

    // Periodic timer that pumps ring -> RT packets. Passive-level is fine; the
    // copy is a plain memcpy and we don't touch hardware.
    WDF_TIMER_CONFIG timerCfg;
    WDF_OBJECT_ATTRIBUTES timerAttribs;
    WDF_TIMER_CONFIG_INIT(&timerCfg, PhonemicEvtPumpTimer);
    timerCfg.Period = 0; // started explicitly in Run with the 10 ms period
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttribs);
    timerAttribs.ParentObject = stream;
    status = WdfTimerCreate(&timerCfg, &timerAttribs, &streamCtx->PumpTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return STATUS_SUCCESS;
}

// Map the user-mode-created named section holding PHONEMIC_RING.
NTSTATUS
PhonemicMapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Stream)
{
    NTSTATUS status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    SIZE_T viewSize = 0;

    if (Stream->Ring != NULL) {
        return STATUS_SUCCESS; // already mapped
    }

    RtlInitUnicodeString(&name, PHONEMIC_RING_SECTION_NAME);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    status = ZwOpenSection(&Stream->SectionHandle, SECTION_MAP_READ, &oa);
    if (!NT_SUCCESS(status)) {
        return status; // user-mode core not running yet
    }

    status = ZwMapViewOfSection(Stream->SectionHandle, ZwCurrentProcess(),
                                &Stream->RingBase, 0, 0, NULL, &viewSize,
                                ViewUnmap, 0, PAGE_READONLY);
    if (!NT_SUCCESS(status)) {
        ZwClose(Stream->SectionHandle);
        Stream->SectionHandle = NULL;
        return status;
    }

    Stream->Ring = (PHONEMIC_RING*)Stream->RingBase;
    if (Stream->Ring->abi_version != PHONEMIC_RING_ABI_VERSION) {
        PhonemicUnmapRing(Stream);
        return STATUS_REVISION_MISMATCH;
    }
    // Start reading from wherever the producer currently is.
    Stream->RingReadIndex = Stream->Ring->write_index;
    return STATUS_SUCCESS;
}

VOID
PhonemicUnmapRing(_Inout_ PPHONEMIC_STREAM_CONTEXT Stream)
{
    if (Stream->RingBase) {
        ZwUnmapViewOfSection(ZwCurrentProcess(), Stream->RingBase);
        Stream->RingBase = NULL;
    }
    if (Stream->SectionHandle) {
        ZwClose(Stream->SectionHandle);
        Stream->SectionHandle = NULL;
    }
    Stream->Ring = NULL;
}

// Allocate the RT capture packets the audio stack will read.
NTSTATUS
PhonemicEvtStreamPrepare(_In_ ACXSTREAM Stream)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    // AcxRtStreamAllocateRtPackets fills ctx->Packets[] with mapped buffers of
    // PHONEMIC_PACKET_BYTES each. (Exact ACX packet-alloc API confirmed at build
    // time against the WDK; the model is: N packets, we fill them round-robin.)
    return AcxRtStreamAllocateRtPackets(Stream, PHONEMIC_PACKET_COUNT,
                                        PHONEMIC_PACKET_BYTES, ctx->Packets);
}

NTSTATUS
PhonemicEvtStreamRun(_In_ ACXSTREAM Stream)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    NTSTATUS status = PhonemicMapRing(ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx->Running = TRUE;
    // Pump a packet every 10 ms to match the producer's frame cadence.
    WdfTimerStart(ctx->PumpTimer, WDF_REL_TIMEOUT_IN_MS(10));
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

// Copy one 10 ms packet's worth of samples from the shared ring into the next RT
// packet; on underrun (producer behind) fill silence so the timeline never
// stalls. Advance CurrentPacket so GetCapturePacket can hand it to the stack.
VOID
PhonemicEvtPumpTimer(_In_ WDFTIMER Timer)
{
    ACXSTREAM stream = (ACXSTREAM)WdfTimerGetParentObject(Timer);
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(stream);
    if (!ctx->Running || ctx->Ring == NULL) {
        return;
    }

    ULONG next = (ctx->CurrentPacket + 1) % PHONEMIC_PACKET_COUNT;
    INT16* dst = (INT16*)ctx->Packets[next];

    // Acquire producer progress before reading samples.
    ULONGLONG write = ctx->Ring->write_index;
    MemoryBarrier();
    ULONGLONG avail = write - ctx->RingReadIndex;

    for (ULONG i = 0; i < PHONEMIC_PACKET_SAMPLES; ++i) {
        if (i < avail) {
            ULONG slot = (ULONG)((ctx->RingReadIndex + i) % PHONEMIC_RING_CAPACITY);
            dst[i] = ctx->Ring->samples[slot];
        } else {
            dst[i] = 0; // underrun -> silence
        }
    }
    ULONGLONG consumed = (avail < PHONEMIC_PACKET_SAMPLES) ? avail : PHONEMIC_PACKET_SAMPLES;
    ctx->RingReadIndex += consumed;
    // Publish our read progress back to the producer.
    MemoryBarrier();
    ctx->Ring->read_index = ctx->RingReadIndex;

    ctx->CurrentPacket = next;
    WdfTimerStart(ctx->PumpTimer, WDF_REL_TIMEOUT_IN_MS(10));
}

// Tell the audio stack which packet is ready and when it was captured.
NTSTATUS
PhonemicEvtStreamGetCapturePacket(
    _In_ ACXSTREAM Stream,
    _Out_ ULONG* LastCapturePacket,
    _Out_ ULONGLONG* QpcPacketStart)
{
    PPHONEMIC_STREAM_CONTEXT ctx = PhonemicStreamGetContext(Stream);
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
    *LastCapturePacket = ctx->CurrentPacket;
    *QpcPacketStart = (ULONGLONG)qpc.QuadPart;
    return STATUS_SUCCESS;
}
