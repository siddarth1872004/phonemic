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
import android.graphics.drawable.LayerDrawable
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
import android.widget.TextView
import android.widget.Toast

/**
 * PhoneMic — main screen. Chrome only: pick the link (Bluetooth / Wi-Fi / USB)
 * and the PC, Voice Focus, Start/Stop/Mute, theme, and live status from the PC.
 * Styled as studio hardware: the [LevelRing] is the Start/Stop key, the
 * other controls are lamp keys, and the accent colour means "live".
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
    private lateinit var signalLabel: TextView
    private lateinit var statusText: TextView
    private lateinit var tally: TextView
    private lateinit var readouts: List<TextView>
    private lateinit var ring: LevelRing
    private lateinit var voiceFocus: Button
    private lateinit var muteButton: Button
    private lateinit var soundboardButton: Button

    private val handler = Handler(Looper.getMainLooper())
    private var startRequestedAt = 0L

    private val poll = object : Runnable {
        override fun run() {
            render()
            handler.postDelayed(this, 150)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        applyAppearance(this)
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
        signalLabel = findViewById(R.id.signalLabel)
        statusText = findViewById(R.id.statusText)
        tally = findViewById(R.id.tally)
        readouts = listOf(R.id.readCodec, R.id.readSent, R.id.readLoss, R.id.readBuffer).map { findViewById(it) }
        ring = findViewById(R.id.levelRing)
        ring.levelSource = { NativeBridge.inputLevel() }
        voiceFocus = findViewById(R.id.voiceFocusKey)
        muteButton = findViewById(R.id.muteButton)
        soundboardButton = findViewById(R.id.soundboardButton)
        findViewById<TextView>(R.id.versionText).text = getString(R.string.version_fmt, BuildConfig.VERSION_NAME)
        // Keep the press ripple inside the key cap.
        (linkButtons.values + themeButtons + listOf(voiceFocus, muteButton, soundboardButton)).forEach { it.clipToOutline = true }

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
        ring.setOnClickListener { onToggle() }
        voiceFocus.setOnClickListener {
            prefs().edit().putBoolean("voice_focus", !voiceFocusOn()).apply()
            render()
        }
        muteButton.setOnClickListener {
            startService(Intent(this, StreamingService::class.java).setAction(StreamingService.ACTION_TOGGLE_MUTE))
        }
        soundboardButton.setOnClickListener { startActivity(Intent(this@MainActivity, SoundboardActivity::class.java)) }
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

    private fun voiceFocusOn() = prefs().getBoolean("voice_focus", true)

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
            .putExtra(StreamingService.EXTRA_VOICE_FOCUS, voiceFocusOn())
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
        val st = StatusText.current()
        val connected = running && st.text == R.string.status_connected
        val micMuted = running && StreamingService.muted
        ring.state = when {
            !running && !pending -> LevelRing.State.IDLE
            micMuted -> LevelRing.State.MUTED
            connected -> LevelRing.State.LIVE
            else -> LevelRing.State.CONNECTING
        }
        ring.title = getString(
            when (ring.state) {
                LevelRing.State.IDLE -> R.string.ring_start
                LevelRing.State.CONNECTING -> R.string.ring_connecting
                LevelRing.State.LIVE -> R.string.ring_live
                LevelRing.State.MUTED -> R.string.ring_muted
            },
        )
        ring.subtitle = getString(if (running) R.string.ring_stop_sub else R.string.ring_start_sub)
        ring.contentDescription = getString(if (running) R.string.stop else R.string.start)
        ring.isEnabled = !pending

        // Tally light: filled only while the PC is actually hearing the mic.
        val onAir = connected && !micMuted
        tally.text = getString(if (onAir) R.string.tally_on_air else if (micMuted) R.string.tally_muted else R.string.tally_standby)
        tally.background = GradientDrawable().apply {
            cornerRadius = dp(5).toFloat()
            when {
                onAir -> setColor(color(R.attr.pmAccent))
                micMuted -> setStroke(dp(1), color(R.attr.pmRed))
                else -> setStroke(dp(1), color(R.attr.pmEdge))
            }
        }
        tally.setTextColor(color(if (onAir) R.attr.pmOnAccent else if (micMuted) R.attr.pmRed else R.attr.pmMuted))

        muteButton.isEnabled = running
        muteButton.text = getString(if (micMuted) R.string.tally_muted else R.string.key_mic)
        lightKey(muteButton, running, if (micMuted) R.attr.pmRed else R.attr.pmGreen)
        voiceFocus.isEnabled = !running
        lightKey(voiceFocus, voiceFocusOn(), R.attr.pmAccent)
        lightKey(soundboardButton, running && StreamingService.soundsMuted, R.attr.pmRed)

        // While streaming, show the link in use; otherwise the one picked.
        val t = if (running) StreamingService.via else transport()
        linkButtons.forEach { (k, b) ->
            lightKey(b, k == t, R.attr.pmAccent)
            b.isEnabled = !running
        }
        themeButtons.forEachIndexed { i, b -> lightKey(b, i == themeIndex(), R.attr.pmAccent) }
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
        statusDot.backgroundTintList = ColorStateList.valueOf(if (connected) tone else color(R.attr.pmLedOff))
        signalLabel.text = getString(if (connected) R.string.signal else R.string.no_signal)
        signalLabel.setTextColor(color(if (connected) R.attr.pmGreen else R.attr.pmMuted))
        statusText.setTextColor(if (st.tone == StatusText.Tone.GOOD) color(R.attr.pmText) else tone)

        if (running) {
            val lost = NativeBridge.pcLost()
            val recv = NativeBridge.pcReceived()
            val lossPct = if (recv + lost > 0) 100.0 * lost / (recv + lost) else 0.0
            val codec = if (NativeBridge.isOpus()) getString(R.string.codec_opus)
            else getString(R.string.codec_pcm, NativeBridge.sampleRate() / 1000.0)
            readouts[0].text = getString(R.string.read_codec, codec)
            readouts[1].text = getString(R.string.read_sent, NativeBridge.packetsSent())
            readouts[2].text = getString(R.string.read_loss, lossPct)
            readouts[3].text = getString(R.string.read_buffer, NativeBridge.pcBufferMs())
            readouts[2].setTextColor(color(if (lossPct > 2.0) R.attr.pmAmber else R.attr.pmText))
        } else {
            val idle = listOf(R.string.read_codec_idle, R.string.read_sent_idle, R.string.read_loss_idle, R.string.read_buffer_idle)
            readouts.forEachIndexed { i, v ->
                v.text = getString(idle[i])
                v.setTextColor(color(R.attr.pmMuted))
            }
        }
    }

    /** Light a key's lamp in [lampAttr], or leave it dark. */
    private fun lightKey(b: Button, lit: Boolean, lampAttr: Int) {
        b.compoundDrawableTintList = ColorStateList.valueOf(color(if (lit) lampAttr else R.attr.pmLedOff))
        b.setTextColor(color(if (lit || b.isEnabled) R.attr.pmText else R.attr.pmMuted))
        b.alpha = if (b.isEnabled) 1f else 0.55f
    }

    private fun themeIndex() = prefs().getInt("theme", 0).coerceIn(0, THEMES.size - 1)
    private fun accentIndex() = prefs().getInt("accent", 0).coerceIn(0, ACCENTS.size - 1)
    private fun accentStyle(i: Int) = if (themeIndex() == THEME_LIGHT) ACCENTS_LIGHT[i] else ACCENTS[i]

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
                // A lamp in a bezel; the chosen one gets a bright bezel.
                background = LayerDrawable(
                    arrayOf(
                        GradientDrawable().apply {
                            shape = GradientDrawable.OVAL
                            setColor(color(R.attr.pmControl))
                            setStroke(dp(if (i == accentIndex()) 2 else 1), color(if (i == accentIndex()) R.attr.pmText else R.attr.pmEdge))
                        },
                        GradientDrawable().apply {
                            shape = GradientDrawable.OVAL
                            setColor(c)
                        },
                    ),
                ).apply { setLayerInset(1, dp(9), dp(9), dp(9), dp(9)) }
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
            R.style.Accent_Signal, R.style.Accent_Teal, R.style.Accent_Violet, R.style.Accent_Rose, R.style.Accent_Ice,
        )
        private val ACCENTS_LIGHT = intArrayOf(
            R.style.Accent_Signal_Light, R.style.Accent_Teal_Light, R.style.Accent_Violet_Light,
            R.style.Accent_Rose_Light, R.style.Accent_Ice_Light,
        )

        /** Theme + accent overlay; must run before the layout is inflated. Shared with [SoundboardActivity]. */
        fun applyAppearance(a: Activity) {
            val p = a.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            val theme = p.getInt("theme", 0).coerceIn(0, THEMES.size - 1)
            val accent = p.getInt("accent", 0).coerceIn(0, ACCENTS.size - 1)
            a.setTheme(THEMES[theme])
            a.theme.applyStyle(if (theme == THEME_LIGHT) ACCENTS_LIGHT[accent] else ACCENTS[accent], true)
        }
    }
}
