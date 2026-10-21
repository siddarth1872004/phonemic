package com.phonemic

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * PhoneMic — main screen. Chrome only: pick the paired PC, Voice Focus,
 * Start/Stop/Mute, and live status from the PC. The audio hot path lives
 * entirely in [StreamingService] -> native code. Framework widgets only, so
 * the APK carries no UI library.
 */
class MainActivity : Activity() {

    private lateinit var pcRow: View
    private lateinit var pcName: TextView
    private lateinit var pcHint: TextView
    private lateinit var pcChevron: ImageView
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var statsText: TextView
    private lateinit var levelBar: ProgressBar
    private lateinit var voiceFocus: Switch
    private lateinit var toggle: Button
    private lateinit var muteButton: Button

    private val handler = Handler(Looper.getMainLooper())
    private var startRequestedAt = 0L

    private val poll = object : Runnable {
        override fun run() {
            render()
            handler.postDelayed(this, 150)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        pcRow = findViewById(R.id.pcRow)
        pcName = findViewById(R.id.pcName)
        pcHint = findViewById(R.id.pcHint)
        pcChevron = findViewById(R.id.pcChevron)
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        statsText = findViewById(R.id.statsText)
        levelBar = findViewById(R.id.levelBar)
        voiceFocus = findViewById(R.id.voiceFocusSwitch)
        toggle = findViewById(R.id.toggleButton)
        muteButton = findViewById(R.id.muteButton)
        findViewById<TextView>(R.id.versionText).text = getString(R.string.version_fmt, BuildConfig.VERSION_NAME)
        // Keep the press ripple inside the pill shape.
        toggle.clipToOutline = true
        muteButton.clipToOutline = true

        voiceFocus.isChecked = prefs().getBoolean("voice_focus", true)

        pcRow.setOnClickListener { withPermissions(REQ_PICK) { choosePc(andStart = false) } }
        toggle.setOnClickListener { onToggle() }
        muteButton.setOnClickListener {
            startService(Intent(this, StreamingService::class.java).setAction(StreamingService.ACTION_TOGGLE_MUTE))
        }
    }

    override fun onResume() {
        super.onResume()
        handler.post(poll)
    }

    override fun onPause() {
        handler.removeCallbacks(poll)
        super.onPause()
    }

    private fun onToggle() {
        if (StreamingService.running) {
            stopService(Intent(this, StreamingService::class.java))
            return
        }
        withPermissions(REQ_START) { startStreaming() }
    }

    /** Run [action] once the microphone and Bluetooth permissions are granted. */
    private fun withPermissions(request: Int, action: () -> Unit) {
        val needed = mutableListOf<String>()
        if (request == REQ_START && !granted(Manifest.permission.RECORD_AUDIO)) needed += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= 31 && !granted(Manifest.permission.BLUETOOTH_CONNECT)) {
            needed += Manifest.permission.BLUETOOTH_CONNECT
        }
        // Optional: the "using your microphone" notification.
        if (request == REQ_START && Build.VERSION.SDK_INT >= 33 && !granted(Manifest.permission.POST_NOTIFICATIONS)) {
            needed += Manifest.permission.POST_NOTIFICATIONS
        }
        if (needed.isEmpty()) action() else requestPermissions(needed.toTypedArray(), request)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, results)
        if (Build.VERSION.SDK_INT >= 31 && !granted(Manifest.permission.BLUETOOTH_CONNECT)) {
            toast(getString(R.string.need_bt))
            return
        }
        when (requestCode) {
            REQ_PICK -> choosePc(andStart = false)
            REQ_START -> if (granted(Manifest.permission.RECORD_AUDIO)) startStreaming() else toast(getString(R.string.need_mic))
        }
    }

    @SuppressLint("MissingPermission")  // BLUETOOTH_CONNECT is granted by withPermissions()
    private fun startStreaming() {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) {
            toast(getString(R.string.no_bt))
            return
        }
        if (!adapter.isEnabled) {
            toast(getString(R.string.bt_off))
            runCatching { startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }
            return
        }
        val address = prefs().getString("pc_address", null)
        if (address == null) {
            choosePc(andStart = true)
            return
        }
        prefs().edit().putBoolean("voice_focus", voiceFocus.isChecked).apply()

        StreamingService.clearError()
        startRequestedAt = System.currentTimeMillis()
        startForegroundService(
            Intent(this, StreamingService::class.java)
                .putExtra(StreamingService.EXTRA_DEVICE, address)
                .putExtra(StreamingService.EXTRA_VOICE_FOCUS, voiceFocus.isChecked),
        )
    }

    /** Pick the PC from the phone's paired devices (computers first). */
    @SuppressLint("MissingPermission")  // only called once BLUETOOTH_CONNECT is granted
    private fun choosePc(andStart: Boolean) {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) {
            toast(getString(R.string.no_bt))
            return
        }
        val devices = runCatching { adapter.bondedDevices.toList() }.getOrDefault(emptyList())
            .sortedWith(
                compareBy(
                    { it.bluetoothClass?.majorDeviceClass != BluetoothClass.Device.Major.COMPUTER },
                    { (it.name ?: it.address).lowercase() },
                ),
            )
        if (devices.isEmpty()) {
            AlertDialog.Builder(this)
                .setTitle(R.string.choose_pc)
                .setMessage(R.string.no_paired_pc)
                .setPositiveButton(R.string.open_bt_settings) { _, _ ->
                    runCatching { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.choose_pc)
            .setItems(devices.map { it.name ?: it.address }.toTypedArray()) { _, i ->
                val d = devices[i]
                prefs().edit().putString("pc_address", d.address).putString("pc_name", d.name ?: d.address).apply()
                render()
                if (andStart) startStreaming()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun render() {
        val running = StreamingService.running
        val pending = !running && StreamingService.lastError == NativeBridge.OK &&
            System.currentTimeMillis() - startRequestedAt < 1500
        toggle.text = getString(if (running) R.string.stop else R.string.start)
        toggle.backgroundTintList = ColorStateList.valueOf(getColor(if (running) R.color.control else R.color.accent))
        toggle.setTextColor(getColor(if (running) R.color.text else R.color.on_accent))
        toggle.isEnabled = !pending
        muteButton.visibility = if (running) View.VISIBLE else View.GONE
        muteButton.text = getString(if (StreamingService.muted) R.string.unmute else R.string.mute)
        muteButton.backgroundTintList = ColorStateList.valueOf(getColor(if (StreamingService.muted) R.color.red else R.color.control))
        voiceFocus.isEnabled = !running

        val name = prefs().getString("pc_name", null)
        pcName.text = name ?: getString(R.string.choose_pc)
        pcName.setTextColor(getColor(if (name == null) R.color.accent else R.color.text))
        pcHint.text = getString(if (name == null) R.string.pc_hint_choose else R.string.pc_hint_paired)
        pcRow.isEnabled = !running
        pcChevron.visibility = if (running) View.INVISIBLE else View.VISIBLE

        val st = StatusText.current()
        statusText.text = if (st.text == R.string.status_connected)
            getString(R.string.status_connected_fmt, NativeBridge.pcBufferMs()) else getString(st.text)
        val color = getColor(
            when (st.tone) {
                StatusText.Tone.GOOD -> R.color.green
                StatusText.Tone.WARN -> R.color.amber
                StatusText.Tone.BAD -> R.color.red
                StatusText.Tone.NEUTRAL -> R.color.muted
            },
        )
        statusDot.backgroundTintList = ColorStateList.valueOf(color)
        statusText.setTextColor(if (st.tone == StatusText.Tone.GOOD) getColor(R.color.text) else color)

        if (running) {
            val level = (NativeBridge.inputLevel() * 100f).toInt().coerceIn(0, 100)
            levelBar.setProgress(level, true)
            val sent = NativeBridge.packetsSent()
            val lost = NativeBridge.pcLost()
            val recv = NativeBridge.pcReceived()
            val lossPct = if (recv + lost > 0) 100.0 * lost / (recv + lost) else 0.0
            val codec = if (NativeBridge.isOpus()) getString(R.string.codec_opus)
            else getString(R.string.codec_pcm, NativeBridge.sampleRate() / 1000.0)
            statsText.text = getString(R.string.stats_fmt, codec, sent, lossPct)
        } else {
            levelBar.setProgress(0, true)
            statsText.text = ""
        }
    }

    private fun prefs() = getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun granted(permission: String) = checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    companion object {
        private const val PREFS = "phonemic"
        private const val REQ_START = 1
        private const val REQ_PICK = 2
    }
}
