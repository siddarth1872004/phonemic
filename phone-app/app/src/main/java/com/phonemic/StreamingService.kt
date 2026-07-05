package com.phonemic

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Foreground service that owns the native streaming engine's lifecycle so mic
 * capture keeps running when the app is backgrounded. It holds no audio state
 * itself — it just starts/stops [NativeBridge].
 */
class StreamingService : Service() {

    private val effects = VoiceEffects()

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val ip = intent?.getStringExtra(EXTRA_IP) ?: return START_NOT_STICKY
        val port = intent.getIntExtra(EXTRA_PORT, DEFAULT_PORT)
        val voiceFocus = intent.getBooleanExtra(EXTRA_VOICE_FOCUS, true)

        startForegroundNotification()

        // Native engine starts capture + UDP send on its own realtime thread.
        if (NativeBridge.start(ip, port)) {
            // Attach the voice effects to the freshly-allocated capture session.
            effects.apply(NativeBridge.sessionId(), voiceFocus)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        effects.release()
        NativeBridge.stop()
        super.onDestroy()
    }

    private fun startForegroundNotification() {
        val channelId = "phonemic_streaming"
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(
                channelId,
                getString(R.string.notif_channel),
                NotificationManager.IMPORTANCE_LOW
            )
        )
        val notif: Notification = Notification.Builder(this, channelId)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notif_text))
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    companion object {
        const val EXTRA_IP = "pc_ip"
        const val EXTRA_PORT = "pc_port"
        const val EXTRA_VOICE_FOCUS = "voice_focus"
        const val DEFAULT_PORT = 4010
        private const val NOTIF_ID = 1
    }
}
