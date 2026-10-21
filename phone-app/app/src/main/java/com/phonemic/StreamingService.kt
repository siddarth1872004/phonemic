package com.phonemic

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.annotation.SuppressLint
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import java.io.IOException
import java.util.UUID

/**
 * Foreground service that owns the native engine's lifecycle so capture keeps
 * running with the screen off, plus the Bluetooth link to the PC.
 *
 * The link is a plain RFCOMM socket to the PC's "PhoneMic" service, made with
 * Android's own Bluetooth stack (the phone and PC are paired, so it is
 * encrypted). One thread connects and writes the engine's encoded frames,
 * another reads the PC's STATUS replies; a dropped link is retried until the
 * user stops. The service is not sticky and the notification can stop/mute.
 */
class StreamingService : Service() {

    private val effects = VoiceEffects()
    private val handler = Handler(Looper.getMainLooper())
    private var voiceFocus = true
    private var effectsGeneration = -1
    private var lastNotifText = ""

    // Owned by this instance (the companion flags outlive it across restarts).
    @Volatile private var active = false
    @Volatile private var socket: BluetoothSocket? = null
    private var linkThread: Thread? = null

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

    enum class Link { CONNECTING, UNREACHABLE, CONNECTED }

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
        // Must be foreground within a few seconds of startForegroundService(),
        // even if it then stops straight away.
        startInForeground(getString(R.string.status_connecting))

        val address = intent?.getStringExtra(EXTRA_DEVICE)
        val device = address?.let { runCatching { bluetoothDevice(it) }.getOrNull() }
        if (intent == null || device == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        voiceFocus = intent.getBooleanExtra(EXTRA_VOICE_FOCUS, true)
        stopLink()

        val rc = NativeBridge.start(voiceFocus)
        if (rc != NativeBridge.OK) {
            lastError = rc
            running = false
            stopSelf()
            return START_NOT_STICKY
        }
        lastError = NativeBridge.OK
        running = true
        muted = false
        NativeBridge.setMuted(false)
        startLink(device)
        effectsGeneration = -1
        handler.removeCallbacks(tick)
        handler.post(tick)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(tick)
        running = false
        effects.release()
        stopLink()
        NativeBridge.stop()
        super.onDestroy()
    }

    // MainActivity holds BLUETOOTH_CONNECT before it starts this service.
    @SuppressLint("MissingPermission")
    private fun bluetoothDevice(address: String): BluetoothDevice? =
        getSystemService(BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)

    private fun startLink(device: BluetoothDevice) {
        active = true
        link = Link.CONNECTING
        linkThread = Thread({ linkLoop(device) }, "phonemic-bt").apply { start() }
    }

    private fun stopLink() {
        active = false
        closeSocket()  // aborts a blocked connect() / read()
        linkThread?.interrupt()
        linkThread = null
    }

    private fun closeSocket() {
        runCatching { socket?.close() }
        socket = null
    }

    @SuppressLint("MissingPermission")
    private fun linkLoop(device: BluetoothDevice) {
        val buf = ByteArray(NativeBridge.MAX_FRAME * 2)
        while (active) {
            link = Link.CONNECTING
            val s = try {
                device.createRfcommSocketToServiceRecord(SERVICE_UUID).also {
                    socket = it
                    it.connect()
                }
            } catch (e: Exception) {  // IOException: PC app not running / out of range
                closeSocket()
                if (!active) break
                link = Link.UNREACHABLE
                if (!pause(RETRY_MS)) break
                continue
            }
            NativeBridge.resetLink()  // nothing stale from before the link was up
            linkedAt = System.currentTimeMillis()
            link = Link.CONNECTED
            val reader = Thread({ readLoop(s) }, "phonemic-bt-in").apply { start() }
            try {
                val out = s.outputStream
                while (active) {
                    val n = NativeBridge.readOutgoing(buf, 200)
                    if (n < 0) break
                    if (n > 0) out.write(buf, 0, n)
                }
            } catch (_: IOException) {
                // the PC closed PhoneMic or walked out of range: reconnect
            }
            closeSocket()
            runCatching { reader.join(500) }
            if (active) {
                link = Link.UNREACHABLE
                if (!pause(1000)) break
            }
        }
    }

    private fun readLoop(s: BluetoothSocket) {
        val buf = ByteArray(512)
        try {
            val input = s.inputStream
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                NativeBridge.feedIncoming(buf, n)
            }
        } catch (_: IOException) {
        }
        runCatching { s.close() }  // the writer's next write fails and reconnects
    }

    private fun pause(ms: Long): Boolean =
        try {
            Thread.sleep(ms)
            active
        } catch (_: InterruptedException) {
            false
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
        /** The PC's Bluetooth address (a paired device). */
        const val EXTRA_DEVICE = "device"
        const val EXTRA_VOICE_FOCUS = "voice_focus"
        const val ACTION_STOP = "com.phonemic.STOP"
        const val ACTION_TOGGLE_MUTE = "com.phonemic.TOGGLE_MUTE"
        private const val CHANNEL_ID = "phonemic_streaming"
        private const val NOTIF_ID = 1
        private const val RETRY_MS = 2000L

        /** The PC app's RFCOMM service — PM_SERVICE_UUID in shared/include/phonemic/wire.h. */
        val SERVICE_UUID: UUID = UUID.fromString("51f009ff-63ac-465d-a8c5-a9a56f7355ea")

        /** Survives activity recreation (rotation etc.) — the UI reads these. */
        @Volatile var running = false
            private set
        @Volatile var muted = false
            private set
        @Volatile var lastError = NativeBridge.OK
            private set
        @Volatile var link = Link.CONNECTING
            private set
        /** When the current link came up (System.currentTimeMillis). */
        @Volatile var linkedAt = 0L
            private set

        fun clearError() {
            lastError = NativeBridge.OK
        }
    }
}
