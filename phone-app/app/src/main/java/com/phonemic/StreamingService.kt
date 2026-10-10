package com.phonemic

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper

/**
 * Foreground service that owns the native engine's lifecycle so capture keeps
 * running with the screen off. It holds no audio itself.
 *
 * Fixes over the previous version: a failed native start is reported (the UI
 * used to show "Streaming" in green regardless), the service is not sticky
 * (a system restart without the IP/PIN could never work), the notification can
 * stop/mute the mic and reflects the PC's state, and a low-latency Wi-Fi lock
 * keeps the radio out of power-save (which adds 50-300 ms bursts of delay).
 */
class StreamingService : Service() {

    private val effects = VoiceEffects()
    private val handler = Handler(Looper.getMainLooper())
    private var wifiLock: WifiManager.WifiLock? = null
    private var voiceFocus = true
    private var effectsGeneration = -1
    private var lastNotifText = ""

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            // Capture stream reopened (headset plugged etc.): re-attach the effects.
            val gen = NativeBridge.streamGeneration()
            if (gen != effectsGeneration) {
                effectsGeneration = gen
                effects.apply(NativeBridge.sessionId(), voiceFocus)
            }
            val text = getString(StatusText.forNotification())
            if (text != lastNotifText) {
                lastNotifText = text
                getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(text))
            }
            handler.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_TOGGLE_MUTE -> {
                muted = !muted
                NativeBridge.setMuted(muted)
                lastNotifText = ""
                return START_NOT_STICKY
            }
        }
        val ip = intent?.getStringExtra(EXTRA_IP)
        if (ip == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        val port = intent.getIntExtra(EXTRA_PORT, Discovery.DEFAULT_PORT)
        voiceFocus = intent.getBooleanExtra(EXTRA_VOICE_FOCUS, true)
        val pin = intent.getStringExtra(EXTRA_PIN) ?: ""

        // Must be foreground within a few seconds of startForegroundService().
        startInForeground(getString(R.string.status_connecting))

        val rc = NativeBridge.start(ip, port, pin, voiceFocus)
        if (rc != NativeBridge.OK) {
            lastError = rc
            running = false
            stopSelf()
            return START_NOT_STICKY
        }
        lastError = NativeBridge.OK
        running = true
        target = "$ip:$port"
        muted = false
        NativeBridge.setMuted(false)
        acquireWifiLock()
        effectsGeneration = -1
        handler.removeCallbacks(tick)
        handler.post(tick)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(tick)
        running = false
        effects.release()
        NativeBridge.stop()
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        super.onDestroy()
    }

    private fun acquireWifiLock() {
        if (wifiLock?.isHeld == true) return
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "phonemic:stream").apply {
            setReferenceCounted(false)
            runCatching { acquire() }
        }
    }

    private fun startInForeground(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW),
        )
        startForeground(NOTIF_ID, buildNotification(text), ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
    }

    private fun buildNotification(text: String): Notification {
        val flags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), flags)
        val stop = PendingIntent.getService(this, 1, Intent(this, StreamingService::class.java).setAction(ACTION_STOP), flags)
        val mute = PendingIntent.getService(this, 2, Intent(this, StreamingService::class.java).setAction(ACTION_TOGGLE_MUTE), flags)
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(if (muted) R.string.notif_title_muted else R.string.notif_title))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_mic)
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(if (muted) R.string.unmute else R.string.mute), mute).build())
            .addAction(Notification.Action.Builder(null, getString(R.string.stop), stop).build())
            .build()
    }

    companion object {
        const val EXTRA_IP = "pc_ip"
        const val EXTRA_PORT = "pc_port"
        const val EXTRA_VOICE_FOCUS = "voice_focus"
        const val EXTRA_PIN = "pin"
        const val ACTION_STOP = "com.phonemic.STOP"
        const val ACTION_TOGGLE_MUTE = "com.phonemic.TOGGLE_MUTE"
        private const val CHANNEL_ID = "phonemic_streaming"
        private const val NOTIF_ID = 1

        /** Survives activity recreation (rotation etc.) — the UI reads these. */
        @Volatile var running = false
            private set
        @Volatile var muted = false
            private set
        @Volatile var lastError = NativeBridge.OK
            private set
        @Volatile var target = ""
            private set

        fun clearError() {
            lastError = NativeBridge.OK
        }
    }
}
