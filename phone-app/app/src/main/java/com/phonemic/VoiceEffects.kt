package com.phonemic

import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor

/**
 * "Voice Focus" — attaches Android's built-in noise suppressor, echo canceller,
 * and automatic gain control to the capture session so the phone cleans up the
 * mic *before* it's sent. All are device-dependent (checked with isAvailable).
 */
class VoiceEffects {
    private var ns: NoiseSuppressor? = null
    private var aec: AcousticEchoCanceler? = null
    private var agc: AutomaticGainControl? = null

    /** Enable the effects on [sessionId] if [enabled]; a no-op session (-1)
     *  or a device lacking an effect is skipped silently. */
    fun apply(sessionId: Int, enabled: Boolean) {
        release()
        if (sessionId < 0 || !enabled) return
        runCatching {
            if (NoiseSuppressor.isAvailable()) {
                ns = NoiseSuppressor.create(sessionId)?.also { it.enabled = true }
            }
            if (AcousticEchoCanceler.isAvailable()) {
                aec = AcousticEchoCanceler.create(sessionId)?.also { it.enabled = true }
            }
            if (AutomaticGainControl.isAvailable()) {
                agc = AutomaticGainControl.create(sessionId)?.also { it.enabled = true }
            }
        }
    }

    fun release() {
        ns?.release(); ns = null
        aec?.release(); aec = null
        agc?.release(); agc = null
    }
}
