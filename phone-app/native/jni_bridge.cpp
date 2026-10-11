// JNI glue between com.phonemic.NativeBridge and the native AudioEngine.
// Only control calls and telemetry cross here — never an audio buffer.
#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "audio_engine.h"

namespace {
std::mutex g_mu;  // start/stop can come from the service and the notification action
std::unique_ptr<phonemic::AudioEngine> g_engine;

phonemic::AudioEngine* engine() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_engine) g_engine = std::make_unique<phonemic::AudioEngine>();
    return g_engine.get();
}

std::string to_string(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_start(JNIEnv* env, jobject, jstring ip, jint port,
                                                            jstring pin, jboolean voice_focus, jboolean opus,
                                                            jboolean usb) {
    if (port <= 0 || port > 65535) return phonemic::AudioEngine::kBadAddress;
    return engine()->start(to_string(env, ip), static_cast<uint16_t>(port), to_string(env, pin),
                           voice_focus == JNI_TRUE, opus == JNI_TRUE, usb == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_phonemic_NativeBridge_stop(JNIEnv*, jobject) { engine()->stop(); }

JNIEXPORT void JNICALL Java_com_phonemic_NativeBridge_setMuted(JNIEnv*, jobject, jboolean m) {
    engine()->set_muted(m == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_com_phonemic_NativeBridge_isRunning(JNIEnv*, jobject) {
    return engine()->running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_phonemic_NativeBridge_packetsSent(JNIEnv*, jobject) {
    return static_cast<jlong>(engine()->packets_sent());
}

JNIEXPORT jfloat JNICALL Java_com_phonemic_NativeBridge_inputLevel(JNIEnv*, jobject) {
    return engine()->input_level();
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_sessionId(JNIEnv*, jobject) {
    return engine()->audio_session_id();
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_streamGeneration(JNIEnv*, jobject) {
    return engine()->stream_generation();
}

JNIEXPORT jboolean JNICALL Java_com_phonemic_NativeBridge_isOpus(JNIEnv*, jobject) {
    return engine()->using_opus() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_sampleRate(JNIEnv*, jobject) {
    return engine()->sample_rate();
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_pcState(JNIEnv*, jobject) { return engine()->pc_state(); }

JNIEXPORT jlong JNICALL Java_com_phonemic_NativeBridge_pcAgeMs(JNIEnv*, jobject) {
    return static_cast<jlong>(engine()->pc_age_ms());
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_pcBufferMs(JNIEnv*, jobject) {
    return engine()->pc_buffer_ms();
}

JNIEXPORT jlong JNICALL Java_com_phonemic_NativeBridge_pcLost(JNIEnv*, jobject) {
    return static_cast<jlong>(engine()->pc_lost());
}

JNIEXPORT jlong JNICALL Java_com_phonemic_NativeBridge_pcReceived(JNIEnv*, jobject) {
    return static_cast<jlong>(engine()->pc_received());
}

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_lastSendErrno(JNIEnv*, jobject) {
    return engine()->last_send_errno();
}

}  // extern "C"
