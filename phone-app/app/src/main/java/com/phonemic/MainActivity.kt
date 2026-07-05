package com.phonemic

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textfield.TextInputEditText

/**
 * PhoneMic — main screen. Chrome only: enter the PC IP, toggle Voice Focus,
 * Start/Stop. The audio hot path lives entirely in [StreamingService] →
 * native code; here we just drive it and show a live level meter.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var ipInput: TextInputEditText
    private lateinit var statusText: TextView
    private lateinit var levelBar: LinearProgressIndicator
    private lateinit var voiceFocus: MaterialSwitch
    private lateinit var toggle: MaterialButton

    private var streaming = false
    private val handler = Handler(Looper.getMainLooper())

    private val requestMic =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) startStreaming() else toast("Microphone permission is required")
        }

    private val poll = object : Runnable {
        override fun run() {
            if (!streaming) return
            val level = (NativeBridge.inputLevel() * 100f).toInt().coerceIn(0, 100)
            levelBar.setProgressCompat(level, true)
            statusText.text = "● Streaming to ${ipInput.text}"
            statusText.setTextColor(getColor(R.color.green))
            handler.postDelayed(this, 120)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ipInput = findViewById(R.id.ipInput)
        statusText = findViewById(R.id.statusText)
        levelBar = findViewById(R.id.levelBar)
        voiceFocus = findViewById(R.id.voiceFocusSwitch)
        toggle = findViewById(R.id.toggleButton)

        // Remember the last IP the user typed.
        val prefs = getPreferences(Context.MODE_PRIVATE)
        ipInput.setText(prefs.getString("ip", ""))

        toggle.setOnClickListener { onToggle() }
    }

    private fun onToggle() {
        if (streaming) {
            stopStreaming()
        } else if (hasMic()) {
            startStreaming()
        } else {
            requestMic.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private fun startStreaming() {
        val ip = ipInput.text?.toString()?.trim().orEmpty()
        if (ip.isEmpty()) {
            toast("Enter the PC IP shown in the PhoneMic window")
            return
        }
        getPreferences(Context.MODE_PRIVATE).edit().putString("ip", ip).apply()

        val intent = Intent(this, StreamingService::class.java).apply {
            putExtra(StreamingService.EXTRA_IP, ip)
            putExtra(StreamingService.EXTRA_PORT, StreamingService.DEFAULT_PORT)
            putExtra(StreamingService.EXTRA_VOICE_FOCUS, voiceFocus.isChecked)
        }
        ContextCompat.startForegroundService(this, intent)

        streaming = true
        toggle.text = "Stop"
        toggle.setBackgroundColor(getColor(R.color.muted))
        ipInput.isEnabled = false
        voiceFocus.isEnabled = false
        handler.postDelayed(poll, 200)
    }

    private fun stopStreaming() {
        stopService(Intent(this, StreamingService::class.java))
        streaming = false
        handler.removeCallbacks(poll)
        toggle.text = "Start"
        toggle.setBackgroundColor(getColor(R.color.accent))
        ipInput.isEnabled = true
        voiceFocus.isEnabled = true
        levelBar.setProgressCompat(0, true)
        statusText.text = "● Not connected"
        statusText.setTextColor(getColor(R.color.muted))
    }

    private fun hasMic() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    override fun onDestroy() {
        handler.removeCallbacks(poll)
        super.onDestroy()
    }
}
