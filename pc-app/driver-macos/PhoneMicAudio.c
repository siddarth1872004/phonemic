/*
 * PhoneMic virtual audio device for macOS — replaces BlackHole.
 *
 * A Core Audio AudioServerPlugIn (user space, loaded by coreaudiod; no kernel
 * extension) that publishes one loopback device, "PhoneMic Microphone":
 *
 *   PhoneMic app ──plays into──► output stream ─┐
 *                                               │ ring indexed by sample time
 *   Zoom / Discord / OBS ◄─records─ input stream ◄┘
 *
 * The desktop app opens the device's output side (it can't be picked as the
 * default output, so the system never sends speaker audio here); other apps
 * record its input side. Both sides run on the device's own clock in the same
 * IO cycle, so no drift handling is needed in here; PhoneMic's adaptive playout
 * already matches the phone's clock to this device's clock.
 *
 * Written against Apple's public AudioServerPlugIn.h API. Formats: 2-channel
 * 32-bit float, 44.1 or 48 kHz.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

#define PM_EXPORT __attribute__((visibility("default")))

// ---- identity -----------------------------------------------------------------

#define kPlugInBundleID    "com.phonemic.audio"
#define kDeviceUID         "PhoneMicLoopback_UID"
#define kDeviceModelUID    "PhoneMicLoopback_ModelUID"
#define kDeviceName        "PhoneMic Microphone"
#define kManufacturer      "PhoneMic"

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,  // 1
    kObjectID_Device = 2,
    kObjectID_Stream_Input = 3,
    kObjectID_Stream_Output = 4,
};

// ---- format / timing ---------------------------------------------------------------

#define kChannels          2u
#define kBytesPerFrame     (kChannels * (UInt32)sizeof(Float32))
#define kZeroTimeStampPeriod 16384u  // frames between the clock's zero time stamps
// Loopback ring (~1.4 s at 48 kHz). Must comfortably exceed the HAL's largest IO
// buffer plus the distance between the output write and input read positions.
#define kRingFrames        65536u

static const Float64 kSupportedRates[] = {44100.0, 48000.0};
#define kNumSupportedRates (sizeof(kSupportedRates) / sizeof(kSupportedRates[0]))

// ---- state ----------------------------------------------------------------------------

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;  // properties
static pthread_mutex_t gIOMutex = PTHREAD_MUTEX_INITIALIZER;     // clock

static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 0;

static Float64 gSampleRate = 48000.0;
static Float64 gPendingSampleRate = 0;  // requested, applied in PerformDeviceConfigurationChange
static UInt64 gIOCount = 0;             // StartIO calls minus StopIO calls
static Boolean gInputActive = true, gOutputActive = true;

static Float64 gHostTicksPerFrame = 0;
static UInt64 gAnchorHostTime = 0;
static UInt64 gNumberTimeStamps = 0;

// Ring: audio plus, per frame, the sample time it was written for. A frame is
// only played back if its tag matches the time being read, so stale audio from
// a previous run (or from long ago, when nobody was recording) is never heard.
static Float32 gRing[kRingFrames * kChannels];
static SInt64 gRingTag[kRingFrames];

static void ComputeHostTicksPerFrame(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const Float64 ticksPerSecond = 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame = ticksPerSecond / gSampleRate;
}

static void ClearRing(void) {
    memset(gRing, 0, sizeof gRing);
    for (UInt32 i = 0; i < kRingFrames; ++i) gRingTag[i] = -1;
}

static Boolean IsSupportedRate(Float64 r) {
    for (size_t i = 0; i < kNumSupportedRates; ++i)
        if (kSupportedRates[i] == r) return true;
    return false;
}

static void FillFormat(AudioStreamBasicDescription* f, Float64 rate) {
    memset(f, 0, sizeof *f);
    f->mSampleRate = rate;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket = kBytesPerFrame;
    f->mFramesPerPacket = 1;
    f->mBytesPerFrame = kBytesPerFrame;
    f->mChannelsPerFrame = kChannels;
    f->mBitsPerChannel = 32;
}

// ---- driver interface (forward declarations) ---------------------------------------

static HRESULT PM_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG PM_AddRef(void* inDriver);
static ULONG PM_Release(void* inDriver);
static OSStatus PM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus PM_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus PM_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus PM_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus PM_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus PM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                    void* inChangeInfo);
static OSStatus PM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                  AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                  void* inChangeInfo);
static Boolean PM_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                              pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus PM_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                      pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                      Boolean* outIsSettable);
static OSStatus PM_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                       UInt32 inQualifierDataSize, const void* inQualifierData,
                                       UInt32* outDataSize);
static OSStatus PM_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                   UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize,
                                   UInt32* outDataSize, void* outData);
static OSStatus PM_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                   UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize,
                                   const void* inData);
static OSStatus PM_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus PM_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus PM_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime,
                                    UInt64* outSeed);
static OSStatus PM_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo,
                                     Boolean* outWillDoInPlace);
static OSStatus PM_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus PM_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                 AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
                                 UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                 void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus PM_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                  const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    PM_QueryInterface,
    PM_AddRef,
    PM_Release,
    PM_Initialize,
    PM_CreateDevice,
    PM_DestroyDevice,
    PM_AddDeviceClient,
    PM_RemoveDeviceClient,
    PM_PerformDeviceConfigurationChange,
    PM_AbortDeviceConfigurationChange,
    PM_HasProperty,
    PM_IsPropertySettable,
    PM_GetPropertyDataSize,
    PM_GetPropertyData,
    PM_SetPropertyData,
    PM_StartIO,
    PM_StopIO,
    PM_GetZeroTimeStamp,
    PM_WillDoIOOperation,
    PM_BeginIOOperation,
    PM_DoIOOperation,
    PM_EndIOOperation,
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// ---- factory (named in Info.plist) ------------------------------------------------------

PM_EXPORT void* PhoneMic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void* PhoneMic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

// ---- IUnknown -------------------------------------------------------------------------------

static HRESULT PM_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) return kAudioHardwareUnspecifiedError;
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        ++gRefCount;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG PM_AddRef(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount < UINT32_MAX) ++gRefCount;
    const ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG PM_Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) --gRefCount;
    const ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;  // the driver object is static: nothing to free
}

// ---- basic operations ---------------------------------------------------------------------

static OSStatus PM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    pthread_mutex_lock(&gStateMutex);
    ComputeHostTicksPerFrame();
    ClearRing();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus PM_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus PM_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus PM_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo* inClientInfo) {
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

static OSStatus PM_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo* inClientInfo) {
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

// The only configuration change we ever request is a sample-rate change; the
// action carries the new rate.
static OSStatus PM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                    void* inChangeInfo) {
    (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    const Float64 rate = (Float64)inChangeAction;
    if (!IsSupportedRate(rate)) return kAudioHardwareBadValueError;
    pthread_mutex_lock(&gStateMutex);
    gSampleRate = rate;
    gPendingSampleRate = 0;
    ComputeHostTicksPerFrame();
    ClearRing();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus PM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                  AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                  void* inChangeInfo) {
    (void)inChangeAction; (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    gPendingSampleRate = 0;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

// ---- property helpers -------------------------------------------------------------------------

static Boolean IsStream(AudioObjectID id) { return id == kObjectID_Stream_Input || id == kObjectID_Stream_Output; }

// Streams of the device visible in `scope` (input, output, or both for global).
static UInt32 DeviceStreams(AudioObjectPropertyScope scope, AudioObjectID out[2]) {
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
        out[n++] = kObjectID_Stream_Input;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
        out[n++] = kObjectID_Stream_Output;
    return n;
}

#define CHECK_SIZE(needed)                                                         \
    do {                                                                           \
        if (inDataSize < (UInt32)(needed)) return kAudioHardwareBadPropertySizeError; \
    } while (0)

static OSStatus PlugInGet(const AudioObjectPropertyAddress* a, UInt32 inQualifierDataSize,
                          const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData,
                          Boolean sizeOnly) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioPlugInClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyOwner:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            CHECK_SIZE(sizeof(AudioObjectID));
            *(AudioObjectID*)outData = kAudioObjectUnknown;
            *outDataSize = sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyManufacturer:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR(kManufacturer);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            *outDataSize = 0;
            if (inDataSize >= sizeof(AudioObjectID)) {
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            }
            return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            CHECK_SIZE(sizeof(AudioObjectID));
            if (inQualifierDataSize < sizeof(CFStringRef) || inQualifierData == NULL)
                return kAudioHardwareBadPropertySizeError;
            {
                CFStringRef uid = *(const CFStringRef*)inQualifierData;
                *(AudioObjectID*)outData =
                    (uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) ? kObjectID_Device
                                                                                            : kAudioObjectUnknown;
            }
            *outDataSize = sizeof(AudioObjectID);
            return 0;
        case kAudioPlugInPropertyResourceBundle:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR("");
            *outDataSize = sizeof(CFStringRef);
            return 0;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus DeviceGet(const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outDataSize,
                          void* outData, Boolean sizeOnly) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioDeviceClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyOwner:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            CHECK_SIZE(sizeof(AudioObjectID));
            *(AudioObjectID*)outData = kObjectID_PlugIn;
            *outDataSize = sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyName:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR(kDeviceName);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioObjectPropertyManufacturer:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR(kManufacturer);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioDevicePropertyDeviceUID:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR(kDeviceUID);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioDevicePropertyModelUID:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = CFSTR(kDeviceModelUID);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioDevicePropertyTransportType:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyRelatedDevices:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            *outDataSize = 0;
            if (inDataSize >= sizeof(AudioObjectID)) {
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            }
            return 0;
        case kAudioDevicePropertyClockDomain:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyDeviceIsAlive:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 1;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyDeviceIsRunning:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            pthread_mutex_lock(&gStateMutex);
            *(UInt32*)outData = gIOCount > 0 ? 1 : 0;
            pthread_mutex_unlock(&gStateMutex);
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            // It may be the default *input* (a microphone), never the default
            // output: the system must not send speaker audio into the mic.
            *(UInt32*)outData = (a->mScope == kAudioObjectPropertyScopeInput) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2];
            const UInt32 n = DeviceStreams(a->mScope, ids);
            if (sizeOnly) { *outDataSize = n * (UInt32)sizeof(AudioObjectID); return 0; }
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioObjectID);
            if (fit > n) fit = n;
            memcpy(outData, ids, fit * sizeof(AudioObjectID));
            *outDataSize = fit * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;  // no volume/mute controls
            return 0;
        case kAudioDevicePropertyNominalSampleRate:
            if (sizeOnly) { *outDataSize = sizeof(Float64); return 0; }
            CHECK_SIZE(sizeof(Float64));
            pthread_mutex_lock(&gStateMutex);
            *(Float64*)outData = gSampleRate;
            pthread_mutex_unlock(&gStateMutex);
            *outDataSize = sizeof(Float64);
            return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            const UInt32 need = (UInt32)(kNumSupportedRates * sizeof(AudioValueRange));
            if (sizeOnly) { *outDataSize = need; return 0; }
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioValueRange);
            if (fit > kNumSupportedRates) fit = (UInt32)kNumSupportedRates;
            AudioValueRange* r = (AudioValueRange*)outData;
            for (UInt32 i = 0; i < fit; ++i) r[i].mMinimum = r[i].mMaximum = kSupportedRates[i];
            *outDataSize = fit * (UInt32)sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyIsHidden:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (sizeOnly) { *outDataSize = 2 * sizeof(UInt32); return 0; }
            CHECK_SIZE(2 * sizeof(UInt32));
            ((UInt32*)outData)[0] = 1;
            ((UInt32*)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 need =
                (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            if (sizeOnly) { *outDataSize = need; return 0; }
            CHECK_SIZE(need);
            AudioChannelLayout* l = (AudioChannelLayout*)outData;
            memset(l, 0, need);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mChannelBitmap = 0;
            l->mNumberChannelDescriptions = kChannels;
            for (UInt32 i = 0; i < kChannels; ++i)
                l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i;
            *outDataSize = need;
            return 0;
        }
        case kAudioDevicePropertyZeroTimeStampPeriod:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = kZeroTimeStampPeriod;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyClockIsStable:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 1;
            *outDataSize = sizeof(UInt32);
            return 0;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus StreamGet(AudioObjectID id, const AudioObjectPropertyAddress* a, UInt32 inDataSize,
                          UInt32* outDataSize, void* outData, Boolean sizeOnly) {
    const Boolean isInput = (id == kObjectID_Stream_Input);
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyClass:
            if (sizeOnly) { *outDataSize = sizeof(AudioClassID); return 0; }
            CHECK_SIZE(sizeof(AudioClassID));
            *(AudioClassID*)outData = kAudioStreamClassID;
            *outDataSize = sizeof(AudioClassID);
            return 0;
        case kAudioObjectPropertyOwner:
            if (sizeOnly) { *outDataSize = sizeof(AudioObjectID); return 0; }
            CHECK_SIZE(sizeof(AudioObjectID));
            *(AudioObjectID*)outData = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return 0;
        case kAudioObjectPropertyName:
            if (sizeOnly) { *outDataSize = sizeof(CFStringRef); return 0; }
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef*)outData = isInput ? CFSTR("PhoneMic Microphone") : CFSTR("PhoneMic feed");
            *outDataSize = sizeof(CFStringRef);
            return 0;
        case kAudioStreamPropertyIsActive:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            pthread_mutex_lock(&gStateMutex);
            *(UInt32*)outData = isInput ? gInputActive : gOutputActive;
            pthread_mutex_unlock(&gStateMutex);
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioStreamPropertyDirection:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = isInput ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioStreamPropertyTerminalType:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioStreamPropertyStartingChannel:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 1;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioStreamPropertyLatency:
            if (sizeOnly) { *outDataSize = sizeof(UInt32); return 0; }
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32*)outData = 0;
            *outDataSize = sizeof(UInt32);
            return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            if (sizeOnly) { *outDataSize = sizeof(AudioStreamBasicDescription); return 0; }
            CHECK_SIZE(sizeof(AudioStreamBasicDescription));
            pthread_mutex_lock(&gStateMutex);
            FillFormat((AudioStreamBasicDescription*)outData, gSampleRate);
            pthread_mutex_unlock(&gStateMutex);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            const UInt32 need = (UInt32)(kNumSupportedRates * sizeof(AudioStreamRangedDescription));
            if (sizeOnly) { *outDataSize = need; return 0; }
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioStreamRangedDescription);
            if (fit > kNumSupportedRates) fit = (UInt32)kNumSupportedRates;
            AudioStreamRangedDescription* d = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < fit; ++i) {
                FillFormat(&d[i].mFormat, kSupportedRates[i]);
                d[i].mSampleRateRange.mMinimum = d[i].mSampleRateRange.mMaximum = kSupportedRates[i];
            }
            *outDataSize = fit * (UInt32)sizeof(AudioStreamRangedDescription);
            return 0;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetProperty(AudioObjectID inObjectID, const AudioObjectPropertyAddress* a,
                            UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize,
                            UInt32* outDataSize, void* outData, Boolean sizeOnly) {
    if (a == NULL || outDataSize == NULL || (!sizeOnly && outData == NULL)) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn)
        return PlugInGet(a, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData, sizeOnly);
    if (inObjectID == kObjectID_Device) return DeviceGet(a, inDataSize, outDataSize, outData, sizeOnly);
    if (IsStream(inObjectID)) return StreamGet(inObjectID, a, inDataSize, outDataSize, outData, sizeOnly);
    return kAudioHardwareBadObjectError;
}

// ---- property dispatch ---------------------------------------------------------------------------

static Boolean PM_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                              pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef || inAddress == NULL) return false;
    UInt32 size = 0;
    // A property exists exactly when we can report its size. TranslateUIDToDevice
    // needs a qualifier for its value, not for its size.
    return GetProperty(inObjectID, inAddress, 0, NULL, 0, &size, NULL, true) == kAudioHardwareNoError;
}

static OSStatus PM_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                      pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                      Boolean* outIsSettable) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outIsSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!PM_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress)) return kAudioHardwareUnknownPropertyError;
    *outIsSettable = false;
    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
        *outIsSettable = true;
    if (IsStream(inObjectID) && (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                                 inAddress->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                 inAddress->mSelector == kAudioStreamPropertyIsActive))
        *outIsSettable = true;
    return kAudioHardwareNoError;
}

static OSStatus PM_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                       UInt32 inQualifierDataSize, const void* inQualifierData,
                                       UInt32* outDataSize) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    return GetProperty(inObjectID, inAddress, inQualifierDataSize, inQualifierData, 0, outDataSize, NULL, true);
}

static OSStatus PM_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                   UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize,
                                   UInt32* outDataSize, void* outData) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    return GetProperty(inObjectID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData,
                       false);
}

// Ask the host to switch the sample rate; the switch happens later in
// PerformDeviceConfigurationChange, when IO is safely stopped.
static OSStatus RequestRate(Float64 rate) {
    if (!IsSupportedRate(rate)) return kAudioDeviceUnsupportedFormatError;
    pthread_mutex_lock(&gStateMutex);
    const Boolean same = (rate == gSampleRate);
    if (!same) gPendingSampleRate = rate;
    pthread_mutex_unlock(&gStateMutex);
    if (same || gHost == NULL) return kAudioHardwareNoError;
    return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
}

static OSStatus PM_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                   UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize,
                                   const void* inData) {
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;

    if (inObjectID == kObjectID_Device) {
        if (inAddress->mSelector != kAudioDevicePropertyNominalSampleRate) return kAudioHardwareUnknownPropertyError;
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return RequestRate(*(const Float64*)inData);
    }
    if (IsStream(inObjectID)) {
        switch (inAddress->mSelector) {
            case kAudioStreamPropertyIsActive: {
                if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                const Boolean on = *(const UInt32*)inData != 0;
                pthread_mutex_lock(&gStateMutex);
                Boolean changed;
                if (inObjectID == kObjectID_Stream_Input) {
                    changed = gInputActive != on;
                    gInputActive = on;
                } else {
                    changed = gOutputActive != on;
                    gOutputActive = on;
                }
                pthread_mutex_unlock(&gStateMutex);
                if (changed && gHost) {
                    AudioObjectPropertyAddress changedAddr = {kAudioStreamPropertyIsActive,
                                                              kAudioObjectPropertyScopeGlobal,
                                                              kAudioObjectPropertyElementMain};
                    gHost->PropertiesChanged(gHost, inObjectID, 1, &changedAddr);
                }
                return kAudioHardwareNoError;
            }
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*)inData;
                AudioStreamBasicDescription ours;
                FillFormat(&ours, f->mSampleRate);
                // Only the sample rate may change; everything else must match.
                if (f->mFormatID != ours.mFormatID || f->mFormatFlags != ours.mFormatFlags ||
                    f->mBytesPerPacket != ours.mBytesPerPacket || f->mFramesPerPacket != ours.mFramesPerPacket ||
                    f->mBytesPerFrame != ours.mBytesPerFrame || f->mChannelsPerFrame != ours.mChannelsPerFrame ||
                    f->mBitsPerChannel != ours.mBitsPerChannel)
                    return kAudioDeviceUnsupportedFormatError;
                return RequestRate(f->mSampleRate);
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kObjectID_PlugIn) return kAudioHardwareUnknownPropertyError;
    return kAudioHardwareBadObjectError;
}

// ---- IO ----------------------------------------------------------------------------------------------

static OSStatus PM_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount == 0) {
        pthread_mutex_lock(&gIOMutex);
        gNumberTimeStamps = 0;
        gAnchorHostTime = mach_absolute_time();
        ClearRing();
        pthread_mutex_unlock(&gIOMutex);
    }
    ++gIOCount;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus PM_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount > 0) --gIOCount;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

// The device clock: a zero time stamp every kZeroTimeStampPeriod frames, laid on
// the host clock from the anchor taken at StartIO.
static OSStatus PM_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime,
                                    UInt64* outSeed) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    const UInt64 now = mach_absolute_time();
    const Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kZeroTimeStampPeriod;
    const UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerPeriod);
    if (next <= now) ++gNumberTimeStamps;
    *outSampleTime = (Float64)(gNumberTimeStamps * kZeroTimeStampPeriod);
    *outHostTime = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerPeriod);
    *outSeed = 1;
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus PM_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo,
                                     Boolean* outWillDoInPlace) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    const Boolean will =
        inOperationID == kAudioServerPlugInIOOperationReadInput || inOperationID == kAudioServerPlugInIOOperationWriteMix;
    if (outWillDo) *outWillDo = will;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus PM_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

// Runs on the device's single IO thread, so the ring needs no locking: the
// output side writes frames tagged with their sample time; the input side
// returns frames whose tag matches the time it reads, silence otherwise.
static OSStatus PM_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                 AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
                                 UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                 void* ioMainBuffer, void* ioSecondaryBuffer) {
    (void)inClientID; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (ioMainBuffer == NULL || inIOCycleInfo == NULL) return kAudioHardwareNoError;
    if (inIOBufferFrameSize > kRingFrames / 2) return kAudioHardwareIllegalOperationError;
    Float32* buf = (Float32*)ioMainBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kObjectID_Stream_Output) {
        const SInt64 t0 = (SInt64)inIOCycleInfo->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i) {
            const SInt64 t = t0 + i;
            const UInt32 slot = (UInt32)((UInt64)t % kRingFrames);
            memcpy(&gRing[slot * kChannels], &buf[i * kChannels], kBytesPerFrame);
            gRingTag[slot] = t;
        }
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kObjectID_Stream_Input) {
        const SInt64 t0 = (SInt64)inIOCycleInfo->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i) {
            const SInt64 t = t0 + i;
            const UInt32 slot = (UInt32)((UInt64)t % kRingFrames);
            if (t >= 0 && gRingTag[slot] == t)
                memcpy(&buf[i * kChannels], &gRing[slot * kChannels], kBytesPerFrame);
            else
                memset(&buf[i * kChannels], 0, kBytesPerFrame);
        }
    }
    return kAudioHardwareNoError;
}

static OSStatus PM_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                  const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}
