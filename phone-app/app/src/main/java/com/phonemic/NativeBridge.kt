package com.phonemic

/**
 * The entire JNI surface between Kotlin and the native audio engine.
 *
 * Principle #1 (the hot path is native): raw audio never crosses this
 * boundary. Capture (Oboe), Opus encoding and framing happen in C++; Kotlin
 * only moves the finished frames onto the Bluetooth socket (Android exposes
 * RFCOMM to Java alone) and hands the PC's replies back. Coarse telemetry
 * comes up for the UI.
 */
object NativeBridge {

    init {
        System.loadLibrary("phonemic")
    }

    // start() results — keep in sync with AudioEngine::Result.
    const val OK = 0
    const val ERR_AUDIO = 3

    // PC feedback states — keep in sync with PM_STATUS_* in shared/include/phonemic/wire.h.
    const val PC_OK = 0
    const val PC_BUSY = 4
    const val PC_UNSUPPORTED = 5

    /** Bytes [readOutgoing] needs room for: one whole frame (PM_FRAME_PREFIX_LEN + PM_MAX_DATAGRAM). */
    const val MAX_FRAME = 2 + 28 + 4096

    /** Open the mic and start producing frames. Returns [OK] or [ERR_AUDIO]. */
    external fun start(voiceFocus: Boolean): Int

    /** Stop and release the mic. Idempotent; a blocked [readOutgoing] returns -1. */
    external fun stop()

    /**
     * Copy whole framed packets ([u16 len][packet]) into [buf], waiting up to
     * [timeoutMs] for the first. Returns bytes copied, 0 on timeout, -1 once stopped.
     */
    external fun readOutgoing(buf: ByteArray, timeoutMs: Int): Int

    /** Bytes received from the PC (its framed STATUS replies), in any chunking. */
    external fun feedIncoming(buf: ByteArray, n: Int)

    /** A new Bluetooth connection: drop the stale backlog and the PC's last report. */
    external fun resetLink()

    external fun setMuted(muted: Boolean)
    external fun isRunning(): Boolean

    /** Packets produced since the last [start]. */
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
}
