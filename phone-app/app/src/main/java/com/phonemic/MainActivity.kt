package com.phonemic

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

/**
 * PhoneMic — main screen. Chrome only: pick the PC (auto-discovery or typed),
 * optional PIN, Voice Focus, Start/Stop/Mute, and live status from the PC.
 * The audio hot path lives entirely in [StreamingService] -> native code.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var ipLayout: TextInputLayout
    private lateinit var ipInput: TextInputEditText
    private lateinit var pinInput: TextInputEditText
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var statsText: TextView
    private lateinit var levelBar: LinearProgressIndicator
    private lateinit var voiceFocus: MaterialSwitch
    private lateinit var opusSwitch: MaterialSwitch
    private lateinit var usbSwitch: MaterialSwitch
    private lateinit var toggle: MaterialButton
    private lateinit var muteButton: MaterialButton

    private val handler = Handler(Looper.getMainLooper())
    private var searching = false
    private var startRequestedAt = 0L

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            // The notification permission is optional; the microphone is not.
            if (result[Manifest.permission.RECORD_AUDIO] == true || hasMic()) startStreaming()
            else toast(getString(R.string.need_mic))
        }

    private val poll = object : Runnable {
        override fun run() {
            render()
            handler.postDelayed(this, 150)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ipLayout = findViewById(R.id.ipLayout)
        ipInput = findViewById(R.id.ipInput)
        pinInput = findViewById(R.id.pinInput)
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        statsText = findViewById(R.id.statsText)
        levelBar = findViewById(R.id.levelBar)
        voiceFocus = findViewById(R.id.voiceFocusSwitch)
        opusSwitch = findViewById(R.id.opusSwitch)
        usbSwitch = findViewById(R.id.usbSwitch)
        toggle = findViewById(R.id.toggleButton)
        muteButton = findViewById(R.id.muteButton)
        findViewById<TextView>(R.id.versionText).text = getString(R.string.version_fmt, BuildConfig.VERSION_NAME)

        val prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        ipInput.setText(prefs.getString("ip", ""))
        pinInput.setText(prefs.getString("pin", ""))
        voiceFocus.isChecked = prefs.getBoolean("voice_focus", true)
        opusSwitch.isChecked = prefs.getBoolean("opus", true)
        usbSwitch.isChecked = prefs.getBoolean("usb", false)
        usbSwitch.setOnCheckedChangeListener { _, _ -> ipLayout.error = null }

        ipLayout.setEndIconOnClickListener { findPc() }
        ipInput.setOnEditorActionListener { _, action, _ ->
            if (action == EditorInfo.IME_ACTION_DONE) { ipLayout.error = null; false } else false
        }
        toggle.setOnClickListener { onToggle() }
        muteButton.setOnClickListener {
            startService(Intent(this, StreamingService::class.java).setAction(StreamingService.ACTION_TOGGLE_MUTE))
        }

        // First run with nothing saved: look for the PC right away.
        if (savedInstanceState == null && ipInput.text.isNullOrBlank() && !StreamingService.running) findPc()
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
        val needed = mutableListOf<String>()
        if (!hasMic()) needed += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) needed += Manifest.permission.POST_NOTIFICATIONS
        if (needed.isEmpty()) startStreaming() else permissionLauncher.launch(needed.toTypedArray())
    }

    /** Accepts "192.168.1.5" or "192.168.1.5:4010". Returns null (and shows an error) if invalid. */
    private fun parseTarget(text: String): Pair<String, Int>? {
        val m = Regex("""^\s*(\d{1,3}(?:\.\d{1,3}){3})(?::(\d{1,5}))?\s*$""").find(text)
        val ip = m?.groupValues?.get(1)
        val port = m?.groupValues?.get(2)?.takeIf { it.isNotEmpty() }?.toIntOrNull() ?: Discovery.DEFAULT_PORT
        if (ip == null || ip.split('.').any { it.toInt() > 255 } || port !in 1..65535) {
            ipLayout.error = getString(R.string.err_ip_format)
            return null
        }
        ipLayout.error = null
        return ip to port
    }

    private fun startStreaming() {
        val usb = usbSwitch.isChecked
        val raw = ipInput.text?.toString().orEmpty()
        if (!usb && raw.isBlank()) {
            findPc()
            return
        }
        val (ip, port) = if (usb) "127.0.0.1" to Discovery.DEFAULT_PORT else parseTarget(raw) ?: return
        val pin = pinInput.text?.toString()?.trim().orEmpty()
        // allowBackup is off, so this stays on the device.
        getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString("ip", raw.trim()).putString("pin", pin)
            .putBoolean("voice_focus", voiceFocus.isChecked)
            .putBoolean("opus", opusSwitch.isChecked)
            .putBoolean("usb", usb).apply()

        StreamingService.clearError()
        startRequestedAt = System.currentTimeMillis()
        val intent = Intent(this, StreamingService::class.java)
            .putExtra(StreamingService.EXTRA_IP, ip)
            .putExtra(StreamingService.EXTRA_PORT, port)
            .putExtra(StreamingService.EXTRA_VOICE_FOCUS, voiceFocus.isChecked)
            .putExtra(StreamingService.EXTRA_PIN, pin)
            .putExtra(StreamingService.EXTRA_OPUS, opusSwitch.isChecked)
            .putExtra(StreamingService.EXTRA_USB, usb)
        ContextCompat.startForegroundService(this, intent)
    }

    private fun findPc() {
        if (searching || StreamingService.running || usbSwitch.isChecked) return
        searching = true
        ipLayout.helperText = getString(R.string.searching)
        Thread {
            val pcs = runCatching { Discovery.find(applicationContext) }.getOrDefault(emptyList())
            runOnUiThread {
                searching = false
                ipLayout.helperText = null
                if (isFinishing || isDestroyed) return@runOnUiThread
                when (pcs.size) {
                    0 -> toast(getString(R.string.no_pc_found))
                    1 -> usePc(pcs[0])
                    else -> AlertDialog.Builder(this)
                        .setTitle(R.string.choose_pc)
                        .setItems(pcs.map { "${it.name}  (${it.ip})" + if (it.busy) " — busy" else "" }.toTypedArray()) { _, i ->
                            usePc(pcs[i])
                        }
                        .show()
                }
            }
        }.start()
    }

    private fun usePc(pc: Discovery.Pc) {
        ipInput.setText(if (pc.port == Discovery.DEFAULT_PORT) pc.ip else "${pc.ip}:${pc.port}")
        ipLayout.error = null
        val msg = getString(R.string.found_pc, pc.name) + if (pc.pinRequired) "\n" + getString(R.string.pc_needs_pin) else ""
        toast(msg)
        if (pc.pinRequired && pinInput.text.isNullOrBlank()) pinInput.requestFocus()
    }

    private fun render() {
        val running = StreamingService.running
        val pending = !running && StreamingService.lastError == NativeBridge.OK &&
            System.currentTimeMillis() - startRequestedAt < 1500
        toggle.text = getString(if (running) R.string.stop else R.string.start)
        toggle.backgroundTintList = ColorStateList.valueOf(getColor(if (running) R.color.control else R.color.accent))
        toggle.isEnabled = !pending
        muteButton.visibility = if (running) View.VISIBLE else View.GONE
        muteButton.text = getString(if (StreamingService.muted) R.string.unmute else R.string.mute)
        muteButton.backgroundTintList = ColorStateList.valueOf(getColor(if (StreamingService.muted) R.color.red else R.color.control))
        ipInput.isEnabled = !running && !usbSwitch.isChecked
        usbSwitch.isEnabled = !running
        pinInput.isEnabled = !running
        voiceFocus.isEnabled = !running
        opusSwitch.isEnabled = !running
        ipLayout.isEndIconVisible = !running && !usbSwitch.isChecked

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
            levelBar.setProgressCompat(level, true)
            val sent = NativeBridge.packetsSent()
            val lost = NativeBridge.pcLost()
            val recv = NativeBridge.pcReceived()
            val lossPct = if (recv + lost > 0) 100.0 * lost / (recv + lost) else 0.0
            val codec = if (NativeBridge.isOpus()) getString(R.string.codec_opus)
            else getString(R.string.codec_pcm, NativeBridge.sampleRate() / 1000.0)
            statsText.text = getString(R.string.stats_fmt, codec, sent, lossPct)
        } else {
            levelBar.setProgressCompat(0, true)
            statsText.text = ""
        }
    }

    private fun hasMic() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    companion object {
        private const val PREFS = "phonemic"
    }
}
