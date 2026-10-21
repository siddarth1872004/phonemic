// JNI glue between com.phonemic.NativeBridge and the native AudioEngine.
// Control calls and telemetry cross here, plus the encoded frames on their way
// to the link's socket (Java owns it) — never a raw audio buffer.
#include <jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include <phonemic/wire.h>

#include "audio_engine.h"

namespace {
std::mutex g_mu;  // start/stop can come from the service and the notification action
std::unique_ptr<phonemic::AudioEngine> g_engine;

phonemic::AudioEngine* engine() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_engine) g_engine = std::make_unique<phonemic::AudioEngine>();
    return g_engine.get();
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_start(JNIEnv* env, jobject, jboolean voice_focus, jstring pin) {
    std::string p;
    if (pin) {
        const char* c = env->GetStringUTFChars(pin, nullptr);
        if (c) {
            p = c;
            env->ReleaseStringUTFChars(pin, c);
        }
    }
    const int r = engine()->start(voice_focus == JNI_TRUE, p);
    std::fill(p.begin(), p.end(), '\0');
    return r;
}

JNIEXPORT void JNICALL Java_com_phonemic_NativeBridge_stop(JNIEnv*, jobject) { engine()->stop(); }

JNIEXPORT jint JNICALL Java_com_phonemic_NativeBridge_readOutgoing(JNIEnv* env, jobject, jbyteArray buf,
                                                                   jint timeout_ms) {
    // Through a stack buffer, so the wait never holds the Java array pinned.
    // `buf` must fit one whole frame (PM_FRAME_PREFIX_LEN + PM_MAX_DATAGRAM).
    jbyte tmp[PM_FRAME_PREFIX_LEN + PM_MAX_DATAGRAM];
    const jsize cap = env->GetArrayLength(buf);
    const int n = engine()->take_outgoing(reinterpret_cast<uint8_t*>(tmp),
                                          std::min<size_t>(sizeof tmp, static_cast<size_t>(cap)), timeout_ms);
    if (n > 0) env->SetByteArrayRegion(buf, 0, n, tmp);
    return n;
}

JNIEXPORT void JNICALL Java_com_phonemic_NativeBridge_feedIncoming(JNIEnv* env, jobject, jbyteArray buf, jint n) {
    if (n <= 0 || n > env->GetArrayLength(buf)) return;
    jbyte tmp[1024];
    for (jint off = 0; off < n;) {
        const jint k = std::min<jint>(n - off, static_cast<jint>(sizeof tmp));
        env->GetByteArrayRegion(buf, off, k, tmp);
        engine()->feed_incoming(reinterpret_cast<const uint8_t*>(tmp), static_cast<size_t>(k));
        off += k;
    }
}

JNIEXPORT void JNICALL Java_com_phonemic_NativeBridge_resetLink(JNIEnv*, jobject) { engine()->reset_link(); }

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

}  // extern "C"
