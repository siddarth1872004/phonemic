package com.phonemic

/** Turns the engine's telemetry into a human status line (shared by the UI and the notification). */
object StatusText {

    enum class Tone { NEUTRAL, GOOD, WARN, BAD }

    data class Status(val text: Int, val tone: Tone)

    fun current(): Status {
        if (!StreamingService.running) {
            return when (StreamingService.lastError) {
                NativeBridge.OK -> Status(R.string.status_idle, Tone.NEUTRAL)
                else -> Status(R.string.err_audio, Tone.BAD)
            }
        }
        when (StreamingService.link) {
            StreamingService.Link.CONNECTING -> return Status(R.string.status_connecting, Tone.NEUTRAL)
            StreamingService.Link.UNREACHABLE -> return Status(R.string.status_unreachable, Tone.WARN)
            StreamingService.Link.CONNECTED -> Unit
        }
        val age = NativeBridge.pcAgeMs()
        if (age in 0..2500) {
            return when (NativeBridge.pcState()) {
                NativeBridge.PC_OK -> Status(R.string.status_connected, Tone.GOOD)
                NativeBridge.PC_BUSY -> Status(R.string.status_busy, Tone.WARN)
                else -> Status(R.string.status_unsupported, Tone.BAD)
            }
        }
        // Linked, but no STATUS yet (just connected) or any more (an old PC app).
        return if (System.currentTimeMillis() - StreamingService.linkedAt > 3000) Status(R.string.status_no_reply, Tone.WARN)
        else Status(R.string.status_waiting, Tone.NEUTRAL)
    }

    fun forNotification(): Int = current().text
}
