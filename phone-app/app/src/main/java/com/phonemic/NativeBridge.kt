package com.phonemic

/**
 * The entire JNI surface between Kotlin and the native audio engine.
 *
 * Design note (principle #1 — hot path is native): the audio buffer never
 * crosses this boundary. Kotlin only tells native *where* to stream and *when*
 * to start/stop; capture (Oboe), framing, and UDP send all happen in C++ on a
 * realtime thread. The only thing that comes back up is coarse UI telemetry
 * (packets sent, input level), never PCM.
 *
 * The project brief's Phase 0 wording ("hand raw PCM frames up to Kotlin") is
 * intentionally NOT followed here: routing every audio buffer through the JVM
 * would put the GC on the hot path, which the non-negotiable principle forbids.
 * See docs/ARCHITECTURE.md.
 */
object NativeBridge {

    init {
        System.loadLibrary("phonemic")
    }

    /**
     * Start capturing the mic and streaming PCM16 to [pcIp]:[pcPort] over UDP.
     * Returns true if the audio stream started. Safe to call once; call [stop]
     * before starting again.
     */
    external fun start(pcIp: String, pcPort: Int): Boolean

    /** Stop streaming and release the audio stream. Idempotent. */
    external fun stop()

    /** Total packets sent since the last [start], for the UI. */
    external fun packetsSent(): Long

    /** Most recent input peak level in [0.0, 1.0], for a level meter. */
    external fun inputLevel(): Float

    /** Audio session id of the running input stream, or -1. Used to attach the
     *  Android voice effects (noise suppressor / echo canceller / auto gain). */
    external fun sessionId(): Int
}
