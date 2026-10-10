package com.phonemic

/**
 * The entire JNI surface between Kotlin and the native audio engine.
 *
 * Principle #1 (the hot path is native): audio buffers never cross this
 * boundary. Kotlin tells native *where* to stream and *when*; capture (Oboe),
 * framing, encryption and UDP all happen in C++. Only coarse telemetry comes
 * back up for the UI.
 */
object NativeBridge {

    init {
        System.loadLibrary("phonemic")
    }

    // start() results — keep in sync with AudioEngine::Result.
    const val OK = 0
    const val ERR_BAD_ADDRESS = 1
    const val ERR_SOCKET = 2
    const val ERR_AUDIO = 3
    const val ERR_CRYPTO = 4

    // PC feedback states — keep in sync with PM_STATUS_* in shared/include/phonemic/wire.h.
    const val PC_OK = 0
    const val PC_PIN_REQUIRED = 1
    const val PC_PIN_MISSING = 2
    const val PC_PIN_WRONG = 3
    const val PC_BUSY = 4
    const val PC_UNSUPPORTED = 5

    /** Open the mic and stream to [pcIp]:[pcPort]. Returns [OK] or an ERR_ code. */
    external fun start(pcIp: String, pcPort: Int, pin: String, voiceFocus: Boolean, opus: Boolean): Int

    /** Stop streaming and release the mic. Idempotent. */
    external fun stop()

    external fun setMuted(muted: Boolean)
    external fun isRunning(): Boolean

    /** Packets sent since the last [start]. */
    external fun packetsSent(): Long

    /** Most recent input peak level in [0.0, 1.0]. */
    external fun inputLevel(): Float

    /** Audio session id of the capture stream (for Android voice effects), or -1. */
    external fun sessionId(): Int

    /** Increments whenever the capture stream is (re)opened, e.g. after a headset change. */
    external fun streamGeneration(): Int

    external fun sampleRate(): Int

    /** True if the running stream is Opus-compressed (false = raw PCM). */
    external fun isOpus(): Boolean

    /** Latest PC feedback state (PC_*), or -1 if the PC hasn't reported yet. */
    external fun pcState(): Int

    /** Milliseconds since the PC last reported, or -1 if never. */
    external fun pcAgeMs(): Long

    external fun pcBufferMs(): Int
    external fun pcLost(): Long
    external fun pcReceived(): Long

    /** errno of the last failed send (0 = sending fine). */
    external fun lastSendErrno(): Int
}
