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
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import java.io.Closeable
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.util.UUID

/**
 * Foreground service that owns the native engine's lifecycle so capture keeps
 * running with the screen off, plus the link to the PC. Three links, one loop:
 *
 *  - Bluetooth: an RFCOMM socket to the PC's "PhoneMic" service through
 *    Android's own stack (paired, so already encrypted).
 *  - Wi-Fi: UDP datagrams to the PC found by [Discovery], one packet each;
 *    sealed with the PIN when the PC asks for one.
 *  - USB: TCP to 127.0.0.1:4010, which `adb reverse` (run by the PC app) carries
 *    over the cable.
 *
 * One thread connects and writes the engine's encoded frames, another reads the
 * PC's STATUS replies; a dropped link is retried until the user stops. The
 * service is not sticky and the notification can stop/mute.
 */
class StreamingService : Service() {

    private val effects = VoiceEffects()
    private val handler = Handler(Looper.getMainLooper())
    private var voiceFocus = true
    private var effectsGeneration = -1
    private var lastNotifText = ""

    // Owned by this instance (the companion flags outlive it across restarts).
    @Volatile private var active = false
    @Volatile private var conn: Conn? = null
    private var linkThread: Thread? = null
    private var wifiLock: WifiManager.WifiLock? = null

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
    enum class Transport { BLUETOOTH, WIFI, USB }

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

        val transport = intent?.getStringExtra(EXTRA_TRANSPORT)
            ?.let { runCatching { Transport.valueOf(it) }.getOrNull() } ?: Transport.BLUETOOTH
        val opener: (() -> Conn)? = when (transport) {
            Transport.BLUETOOTH -> intent?.getStringExtra(EXTRA_DEVICE)
                ?.let { runCatching { bluetoothDevice(it) }.getOrNull() }
                ?.let { d -> { BluetoothConn(d) } }
            Transport.WIFI -> intent?.getStringExtra(EXTRA_HOST)?.let { host ->
                val port = intent.getIntExtra(EXTRA_PORT, Discovery.DEFAULT_PORT)
                ({ WifiConn(host, port) })
            }
            Transport.USB -> ({ UsbConn() })
        }
        if (intent == null || opener == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        voiceFocus = intent.getBooleanExtra(EXTRA_VOICE_FOCUS, true)
        // Only Wi-Fi needs the PIN: Bluetooth and USB are private links already.
        val pin = if (transport == Transport.WIFI) intent.getStringExtra(EXTRA_PIN).orEmpty() else ""
        stopLink()

        val rc = NativeBridge.start(voiceFocus, pin)
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
        via = transport
        if (transport == Transport.WIFI) acquireWifiLock() else releaseWifiLock()
        startLink(opener)
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
        releaseWifiLock()
        NativeBridge.stop()
        super.onDestroy()
    }

    // MainActivity holds BLUETOOTH_CONNECT before it starts this service.
    @SuppressLint("MissingPermission")
    private fun bluetoothDevice(address: String): BluetoothDevice? =
        getSystemService(BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)

    /** A Wi-Fi radio in power save adds 100+ ms of jitter; hold it awake while streaming. */
    private fun acquireWifiLock() {
        if (wifiLock?.isHeld == true) return
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "phonemic:stream").apply {
            setReferenceCounted(false)
            runCatching { acquire() }
        }
    }

    private fun releaseWifiLock() {
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    private fun startLink(open: () -> Conn) {
        active = true
        link = Link.CONNECTING
        linkThread = Thread({ linkLoop(open) }, "phonemic-link").apply { start() }
    }

    private fun stopLink() {
        active = false
        closeConn()  // aborts a blocked connect() / read()
        linkThread?.interrupt()
        linkThread = null
    }

    private fun closeConn() {
        runCatching { conn?.close() }
        conn = null
    }

    private fun linkLoop(open: () -> Conn) {
        val buf = ByteArray(NativeBridge.MAX_FRAME * 2)
        while (active) {
            link = Link.CONNECTING
            val c = try {
                open().also {
                    conn = it
                    it.connect()
                }
            } catch (e: Exception) {  // PC app not running / out of range / no cable
                closeConn()
                if (!active) break
                link = Link.UNREACHABLE
                if (!pause(RETRY_MS)) break
                continue
            }
            NativeBridge.resetLink()  // nothing stale from before the link was up
            linkedAt = System.currentTimeMillis()
            link = Link.CONNECTED
            val reader = Thread({ readLoop(c) }, "phonemic-link-in").apply { start() }
            try {
                while (active) {
                    val n = NativeBridge.readOutgoing(buf, 200)
                    if (n < 0) break
                    if (n > 0) c.write(buf, n)
                }
            } catch (_: IOException) {
                // the PC closed PhoneMic, walked out of range or the cable came out: reconnect
            }
            closeConn()
            runCatching { reader.join(500) }
            if (active) {
                link = Link.UNREACHABLE
                if (!pause(1000)) break
            }
        }
    }

    private fun readLoop(c: Conn) {
        val buf = ByteArray(512)
        try {
            while (true) {
                val n = c.read(buf)
                if (n < 0) break
                NativeBridge.feedIncoming(buf, n)
            }
        } catch (_: IOException) {
        }
        runCatching { c.close() }  // the writer's next write fails and reconnects
    }

    /** One link to the PC. [write] takes whole framed packets; [read] returns framed STATUS bytes. */
    private interface Conn : Closeable {
        fun connect()
        fun write(buf: ByteArray, n: Int)
        fun read(buf: ByteArray): Int
    }

    @SuppressLint("MissingPermission")
    private class BluetoothConn(private val device: BluetoothDevice) : Conn {
        private var s: BluetoothSocket? = null
        override fun connect() {
            s = device.createRfcommSocketToServiceRecord(SERVICE_UUID)
            s!!.connect()
        }
        override fun write(buf: ByteArray, n: Int) = s!!.outputStream.write(buf, 0, n)
        override fun read(buf: ByteArray): Int = s!!.inputStream.read(buf)
        override fun close() { s?.close() }
    }

    /** USB: `adb reverse` maps the phone's 127.0.0.1:4010 to PhoneMic on the PC. */
    private class UsbConn : Conn {
        private val s = Socket()
        override fun connect() {
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(InetAddress.getLoopbackAddress(), Discovery.DEFAULT_PORT), 2000)
        }
        override fun write(buf: ByteArray, n: Int) = s.getOutputStream().write(buf, 0, n)
        override fun read(buf: ByteArray): Int = s.getInputStream().read(buf)
        override fun close() = s.close()
    }

    /** Wi-Fi: one UDP datagram per packet, so the length prefixes come off here and go back on for replies. */
    private class WifiConn(private val host: String, private val port: Int) : Conn {
        private val s = DatagramSocket()
        private val rx = ByteArray(512)
        override fun connect() = s.connect(InetSocketAddress(InetAddress.getByName(host), port))
        override fun write(buf: ByteArray, n: Int) {
            var off = 0
            while (off + 2 <= n) {
                val len = (buf[off].toInt() and 0xFF) or ((buf[off + 1].toInt() and 0xFF) shl 8)
                if (off + 2 + len > n) break
                s.send(DatagramPacket(buf, off + 2, len))
                off += 2 + len
            }
        }
        override fun read(buf: ByteArray): Int {
            val p = DatagramPacket(rx, rx.size)
            s.receive(p)
            if (p.length > buf.size - 2) return 0
            buf[0] = p.length.toByte()
            buf[1] = (p.length shr 8).toByte()
            System.arraycopy(rx, 0, buf, 2, p.length)
            return p.length + 2
        }
        override fun close() = s.close()
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
        /** A [Transport] name. */
        const val EXTRA_TRANSPORT = "transport"
        /** Bluetooth: the PC's address (a paired device). */
        const val EXTRA_DEVICE = "device"
        /** Wi-Fi: the PC's IP address and port, and the PIN (if it wants one). */
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_PIN = "pin"
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
        @Volatile var via = Transport.BLUETOOTH
            private set
        /** When the current link came up (System.currentTimeMillis). */
        @Volatile var linkedAt = 0L
            private set

        fun clearError() {
            lastError = NativeBridge.OK
        }
    }
}
