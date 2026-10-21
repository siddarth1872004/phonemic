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
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.util.TypedValue
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * PhoneMic — main screen. Chrome only: pick the link (Bluetooth / Wi-Fi / USB)
 * and the PC, Voice Focus, Start/Stop/Mute, theme, and live status from the PC.
 * The audio hot path lives entirely in [StreamingService] -> native code.
 * Framework widgets only, so the APK carries no UI library.
 */
class MainActivity : Activity() {

    private lateinit var linkButtons: Map<StreamingService.Transport, Button>
    private lateinit var themeButtons: List<Button>
    private lateinit var accentRow: LinearLayout
    private lateinit var pinRow: View
    private lateinit var pinValue: TextView
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
        applyAppearance()
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        linkButtons = mapOf(
            StreamingService.Transport.BLUETOOTH to findViewById(R.id.linkBluetooth),
            StreamingService.Transport.WIFI to findViewById(R.id.linkWifi),
            StreamingService.Transport.USB to findViewById(R.id.linkUsb),
        )
        themeButtons = listOf(findViewById(R.id.themeOled), findViewById(R.id.themeDark), findViewById(R.id.themeLight))
        accentRow = findViewById(R.id.accentRow)
        pinRow = findViewById(R.id.pinRow)
        pinValue = findViewById(R.id.pinValue)
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
        (linkButtons.values + themeButtons).forEach { it.clipToOutline = true }

        voiceFocus.isChecked = prefs().getBoolean("voice_focus", true)

        linkButtons.forEach { (t, b) ->
            b.setOnClickListener {
                prefs().edit().putString("transport", t.name).apply()
                render()
            }
        }
        themeButtons.forEachIndexed { i, b -> b.setOnClickListener { setAppearance("theme", i) } }
        buildAccentRow()
        pinRow.setOnClickListener { askPin() }
        pcRow.setOnClickListener {
            when (transport()) {
                StreamingService.Transport.BLUETOOTH -> withPermissions(REQ_PICK) { choosePc(andStart = false) }
                StreamingService.Transport.WIFI -> findWifiPc(andStart = false)
                StreamingService.Transport.USB -> Unit
            }
        }
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

    private fun transport(): StreamingService.Transport =
        runCatching { StreamingService.Transport.valueOf(prefs().getString("transport", null)!!) }
            .getOrDefault(StreamingService.Transport.BLUETOOTH)

    private fun needsBluetooth() = Build.VERSION.SDK_INT >= 31 &&
        transport() == StreamingService.Transport.BLUETOOTH && !granted(Manifest.permission.BLUETOOTH_CONNECT)

    /** Run [action] once the microphone (and, for Bluetooth, Nearby devices) permissions are granted. */
    private fun withPermissions(request: Int, action: () -> Unit) {
        val needed = mutableListOf<String>()
        if (request == REQ_START && !granted(Manifest.permission.RECORD_AUDIO)) needed += Manifest.permission.RECORD_AUDIO
        if (needsBluetooth()) {
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
        if (needsBluetooth()) {
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
        val t = transport()
        val intent = Intent(this, StreamingService::class.java)
            .putExtra(StreamingService.EXTRA_TRANSPORT, t.name)
            .putExtra(StreamingService.EXTRA_VOICE_FOCUS, voiceFocus.isChecked)
        when (t) {
            StreamingService.Transport.BLUETOOTH -> {
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
                intent.putExtra(StreamingService.EXTRA_DEVICE, address)
            }
            StreamingService.Transport.WIFI -> {
                val host = prefs().getString("wifi_host", null)
                if (host == null) {
                    findWifiPc(andStart = true)
                    return
                }
                intent.putExtra(StreamingService.EXTRA_HOST, host)
                    .putExtra(StreamingService.EXTRA_PORT, prefs().getInt("wifi_port", Discovery.DEFAULT_PORT))
                    .putExtra(StreamingService.EXTRA_PIN, prefs().getString("wifi_pin", "").orEmpty())
            }
            StreamingService.Transport.USB -> Unit
        }
        prefs().edit().putBoolean("voice_focus", voiceFocus.isChecked).apply()

        StreamingService.clearError()
        startRequestedAt = System.currentTimeMillis()
        startForegroundService(intent)
    }

    /** Ask the PCs on this Wi-Fi to announce themselves (discovery), then pick one. */
    private fun findWifiPc(andStart: Boolean) {
        val searching = AlertDialog.Builder(this)
            .setTitle(R.string.find_pc)
            .setMessage(R.string.searching)
            .setNegativeButton(R.string.cancel, null)
            .show()
        val app = applicationContext
        Thread({
            val pcs = runCatching { Discovery.find(app) }.getOrDefault(emptyList())
            runOnUiThread {
                if (isFinishing || isDestroyed || !searching.isShowing) return@runOnUiThread
                searching.dismiss()
                showWifiPcs(pcs, andStart)
            }
        }, "phonemic-discovery").start()
    }

    private fun showWifiPcs(pcs: List<Discovery.Pc>, andStart: Boolean) {
        val labels = pcs.map {
            val name = if (it.pinRequired) getString(R.string.pc_pin_suffix, it.name) else it.name
            if (it.busy) getString(R.string.pc_busy_suffix, name) else name
        } + getString(R.string.enter_address)
        val b = AlertDialog.Builder(this).setTitle(R.string.find_pc)
        if (pcs.isEmpty()) b.setMessage(R.string.no_wifi_pc)
            .setPositiveButton(R.string.enter_address) { _, _ -> askAddress(andStart) }
        else b.setItems(labels.toTypedArray()) { _, i ->
            if (i == pcs.size) {
                askAddress(andStart)
                return@setItems
            }
            val pc = pcs[i]
            saveWifiPc(pc.name, pc.ip, pc.port)
            if (pc.pinRequired && prefs().getString("wifi_pin", "").isNullOrEmpty()) askPin(andStart)
            else if (andStart) startStreaming()
        }
        b.setNeutralButton(R.string.search_again) { _, _ -> findWifiPc(andStart) }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /** Type the PC's address when discovery can't reach it (guest networks, VPNs, other subnets). */
    private fun askAddress(andStart: Boolean) {
        val input = dialogEdit(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI, getString(R.string.address_hint))
        prefs().getString("wifi_host", null)?.let { input.setText(it) }
        AlertDialog.Builder(this)
            .setTitle(R.string.enter_address)
            .setView(padded(input))
            .setPositiveButton(R.string.ok) { _, _ ->
                val text = input.text.toString().trim()
                val host = text.substringBeforeLast(':', text)
                val port = if (':' in text) text.substringAfterLast(':').toIntOrNull() else Discovery.DEFAULT_PORT
                if (host.isEmpty() || ' ' in host || port == null || port !in 1..65535) {
                    toast(getString(R.string.bad_address))
                    return@setPositiveButton
                }
                saveWifiPc(host, host, port)
                if (andStart) startStreaming()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun saveWifiPc(name: String, host: String, port: Int) {
        prefs().edit().putString("wifi_name", name).putString("wifi_host", host).putInt("wifi_port", port).apply()
        render()
    }

    /** The PIN set in PhoneMic on the PC; Wi-Fi audio is sealed with it. Empty = no PIN. */
    private fun askPin(andStart: Boolean = false) {
        val input = dialogEdit(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD, getString(R.string.pin_label))
        input.setText(prefs().getString("wifi_pin", ""))
        AlertDialog.Builder(this)
            .setTitle(R.string.pin_title)
            .setMessage(R.string.pin_desc)
            .setView(padded(input))
            .setPositiveButton(R.string.ok) { _, _ ->
                prefs().edit().putString("wifi_pin", input.text.toString()).apply()
                render()
                if (andStart) startStreaming()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun dialogEdit(type: Int, hint: String) = EditText(this).apply {
        inputType = type
        this.hint = hint
        isSingleLine = true
        setTextColor(color(R.attr.pmText))
        backgroundTintList = ColorStateList.valueOf(color(R.attr.pmAccent))
    }

    private fun padded(v: View) = FrameLayout(this).apply {
        setPadding(dp(22), dp(8), dp(22), 0)
        addView(v)
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
        toggle.backgroundTintList = ColorStateList.valueOf(color(if (running) R.attr.pmControl else R.attr.pmAccent))
        toggle.setTextColor(color(if (running) R.attr.pmText else R.attr.pmOnAccent))
        toggle.isEnabled = !pending
        muteButton.visibility = if (running) View.VISIBLE else View.GONE
        muteButton.text = getString(if (StreamingService.muted) R.string.unmute else R.string.mute)
        muteButton.backgroundTintList = ColorStateList.valueOf(color(if (StreamingService.muted) R.attr.pmRed else R.attr.pmControl))
        voiceFocus.isEnabled = !running

        // While streaming, show the link in use; otherwise the one picked.
        val t = if (running) StreamingService.via else transport()
        linkButtons.forEach { (k, b) ->
            tintSegment(b, k == t)
            b.isEnabled = !running
        }
        themeButtons.forEachIndexed { i, b -> tintSegment(b, i == themeIndex()) }
        val name: String?
        when (t) {
            StreamingService.Transport.BLUETOOTH -> {
                name = prefs().getString("pc_name", null)
                pcName.text = name ?: getString(R.string.choose_pc)
                pcHint.text = getString(if (name == null) R.string.pc_hint_choose else R.string.pc_hint_paired)
            }
            StreamingService.Transport.WIFI -> {
                name = prefs().getString("wifi_name", null)
                val host = prefs().getString("wifi_host", null)
                val port = prefs().getInt("wifi_port", Discovery.DEFAULT_PORT)
                pcName.text = name ?: getString(R.string.find_pc)
                pcHint.text = if (host == null) getString(R.string.wifi_hint_choose)
                else getString(R.string.wifi_hint_chosen, if (port == Discovery.DEFAULT_PORT) host else "$host:$port")
            }
            StreamingService.Transport.USB -> {
                name = getString(R.string.usb_name)
                pcName.text = name
                pcHint.text = getString(R.string.usb_hint)
            }
        }
        pcName.setTextColor(color(if (name == null) R.attr.pmAccent else R.attr.pmText))
        val pickable = !running && t != StreamingService.Transport.USB
        pcRow.isEnabled = pickable
        pcChevron.visibility = if (pickable) View.VISIBLE else View.INVISIBLE
        pinRow.visibility = if (t == StreamingService.Transport.WIFI) View.VISIBLE else View.GONE
        pinRow.isEnabled = !running
        pinValue.text = getString(if (prefs().getString("wifi_pin", "").isNullOrEmpty()) R.string.pin_none else R.string.pin_set)

        val st = StatusText.current()
        statusText.text = if (st.text == R.string.status_connected)
            getString(R.string.status_connected_fmt, NativeBridge.pcBufferMs()) else getString(st.text)
        val tone = color(
            when (st.tone) {
                StatusText.Tone.GOOD -> R.attr.pmGreen
                StatusText.Tone.WARN -> R.attr.pmAmber
                StatusText.Tone.BAD -> R.attr.pmRed
                StatusText.Tone.NEUTRAL -> R.attr.pmMuted
            },
        )
        statusDot.backgroundTintList = ColorStateList.valueOf(tone)
        statusText.setTextColor(if (st.tone == StatusText.Tone.GOOD) color(R.attr.pmText) else tone)

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

    private fun tintSegment(b: Button, selected: Boolean) {
        b.backgroundTintList = ColorStateList.valueOf(color(if (selected) R.attr.pmAccent else R.attr.pmControl))
        b.setTextColor(color(if (selected) R.attr.pmOnAccent else R.attr.pmText))
    }

    private fun themeIndex() = prefs().getInt("theme", 0).coerceIn(0, THEMES.size - 1)
    private fun accentIndex() = prefs().getInt("accent", 0).coerceIn(0, ACCENTS.size - 1)
    private fun accentStyle(i: Int) = if (themeIndex() == THEME_LIGHT) ACCENTS_LIGHT[i] else ACCENTS[i]

    /** Theme + accent overlay; must run before the layout is inflated. */
    private fun applyAppearance() {
        setTheme(THEMES[themeIndex()])
        theme.applyStyle(accentStyle(accentIndex()), true)
    }

    private fun setAppearance(key: String, value: Int) {
        if (prefs().getInt(key, 0) == value) return
        prefs().edit().putInt(key, value).apply()
        recreate()
    }

    /** One swatch per accent, in this theme's shade; the chosen one is ringed. */
    private fun buildAccentRow() {
        accentRow.removeAllViews()
        for (i in ACCENTS.indices) {
            val a = obtainStyledAttributes(accentStyle(i), intArrayOf(R.attr.pmAccent))
            val c = a.getColor(0, 0)
            a.recycle()
            val swatch = View(this).apply {
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(c)
                    if (i == accentIndex()) setStroke(dp(3), color(R.attr.pmText))
                }
                contentDescription = resources.getResourceEntryName(ACCENTS[i]).substringAfter('.')
                setOnClickListener { setAppearance("accent", i) }
            }
            accentRow.addView(swatch, LinearLayout.LayoutParams(dp(34), dp(34)).apply { marginEnd = dp(14) })
        }
    }

    private fun color(attr: Int): Int {
        val v = TypedValue()
        theme.resolveAttribute(attr, v, true)
        return v.data
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun prefs() = getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun granted(permission: String) = checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    companion object {
        private const val PREFS = "phonemic"
        private const val REQ_START = 1
        private const val REQ_PICK = 2

        // Same order as the desktop app's themes and accents (pc-app/gui/ui.cpp).
        private val THEMES = intArrayOf(R.style.Theme_PhoneMic, R.style.Theme_PhoneMic_Dark, R.style.Theme_PhoneMic_Light)
        private const val THEME_LIGHT = 2
        private val ACCENTS = intArrayOf(
            R.style.Accent_Periwinkle, R.style.Accent_Teal, R.style.Accent_Violet, R.style.Accent_Rose, R.style.Accent_Amber,
        )
        private val ACCENTS_LIGHT = intArrayOf(
            R.style.Accent_Periwinkle_Light, R.style.Accent_Teal_Light, R.style.Accent_Violet_Light,
            R.style.Accent_Rose_Light, R.style.Accent_Amber_Light,
        )
    }
}
