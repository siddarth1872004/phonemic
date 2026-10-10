package com.phonemic

import androidx.annotation.StringRes

/** Turns the engine's telemetry into a human status line (shared by the UI and the notification). */
object StatusText {

    enum class Tone { NEUTRAL, GOOD, WARN, BAD }

    data class Status(@StringRes val text: Int, val tone: Tone)

    private const val ENETUNREACH = 101
    private const val ECONNREFUSED = 111
    private const val EHOSTUNREACH = 113

    fun current(): Status {
        if (!StreamingService.running) {
            return when (StreamingService.lastError) {
                NativeBridge.OK -> Status(R.string.status_idle, Tone.NEUTRAL)
                NativeBridge.ERR_BAD_ADDRESS -> Status(R.string.err_bad_address, Tone.BAD)
                NativeBridge.ERR_AUDIO -> Status(R.string.err_audio, Tone.BAD)
                NativeBridge.ERR_CRYPTO -> Status(R.string.err_crypto, Tone.BAD)
                else -> Status(R.string.err_socket, Tone.BAD)
            }
        }
        when (NativeBridge.lastSendErrno()) {
            ENETUNREACH, EHOSTUNREACH -> return Status(R.string.status_no_network, Tone.BAD)
            ECONNREFUSED -> return Status(R.string.status_pc_not_running, Tone.BAD)
        }
        val age = NativeBridge.pcAgeMs()
        if (age in 0..2500) {
            return when (NativeBridge.pcState()) {
                NativeBridge.PC_OK -> Status(R.string.status_connected, Tone.GOOD)
                NativeBridge.PC_PIN_REQUIRED -> Status(R.string.status_pin_required, Tone.BAD)
                NativeBridge.PC_PIN_MISSING -> Status(R.string.status_pin_missing, Tone.BAD)
                NativeBridge.PC_PIN_WRONG -> Status(R.string.status_pin_wrong, Tone.BAD)
                NativeBridge.PC_BUSY -> Status(R.string.status_busy, Tone.WARN)
                else -> Status(R.string.status_unsupported, Tone.BAD)
            }
        }
        // Sending, but the PC never answered (wrong IP, firewall, or an old PC app).
        return if (NativeBridge.packetsSent() > 300) Status(R.string.status_no_reply, Tone.WARN)
        else Status(R.string.status_connecting, Tone.NEUTRAL)
    }

    @StringRes
    fun forNotification(): Int = current().text
}
