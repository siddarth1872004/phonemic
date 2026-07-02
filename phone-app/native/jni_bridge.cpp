// JNI glue between com.phonemic.NativeBridge and the native AudioEngine.
// This file only marshals control calls; no audio buffer ever crosses here.
#include <jni.h>

#include <memory>
#include <string>

#include "audio_engine.h"

namespace {
// One engine per process. Guarded only by the fact that start/stop are driven
// from the single StreamingService lifecycle on the main thread.
std::unique_ptr<phonemic::AudioEngine> g_engine;
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_phonemic_NativeBridge_start(JNIEnv* env, jobject, jstring j_ip, jint port) {
    const char* ip_c = env->GetStringUTFChars(j_ip, nullptr);
    std::string ip(ip_c ? ip_c : "");
    env->ReleaseStringUTFChars(j_ip, ip_c);

    if (!g_engine) g_engine = std::make_unique<phonemic::AudioEngine>();
    return g_engine->start(ip, static_cast<uint16_t>(port)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_phonemic_NativeBridge_stop(JNIEnv*, jobject) {
    if (g_engine) g_engine->stop();
}

JNIEXPORT jlong JNICALL
Java_com_phonemic_NativeBridge_packetsSent(JNIEnv*, jobject) {
    return g_engine ? static_cast<jlong>(g_engine->packets_sent()) : 0;
}

JNIEXPORT jfloat JNICALL
Java_com_phonemic_NativeBridge_inputLevel(JNIEnv*, jobject) {
    return g_engine ? g_engine->input_level() : 0.0f;
}

}  // extern "C"
