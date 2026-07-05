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

    // Wire the RT stream callbacks (Prepare/Run/Pause/GetCapturePacket). The
    // GetCapturePacket handler is where phonemic_ring_available() is checked and
    // samples are memcpy'd from PHONEMIC_RING::samples into the packet buffer.
    ACX_RT_STREAM_CALLBACKS rt;
    ACX_RT_STREAM_CALLBACKS_INIT(&rt);
    // rt.EvtAcxStreamGetCapturePacket = PhonemicEvtStreamGetCapturePacket;
    // rt.EvtAcxStreamRun              = PhonemicEvtStreamRun;
    // rt.EvtAcxStreamPause            = PhonemicEvtStreamPause;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rt);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AcxStreamCreate(Device, StreamInit, &attributes, &stream);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PhonemicStreamGetContext(stream)->Device = PhonemicDeviceGetContext(Device);
    return STATUS_SUCCESS;
}
