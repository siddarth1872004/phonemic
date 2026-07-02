package com.phonemic

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Minimal Phase 0 UI: enter the PC's IP, grant mic permission, start/stop.
 * All real work happens in [StreamingService] → [NativeBridge]. This class is
 * chrome only — no audio buffers here.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var ipField: EditText
    private lateinit var toggle: Button
    private var streaming = false

    private val requestMic =
        registerForActivityResult(
            androidx.activity.result.contract.ActivityResultContracts.RequestPermission()
        ) { granted ->
            if (granted) startStreaming() else toast("Microphone permission is required")
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        ipField = EditText(this).apply {
            hint = getString(R.string.hint_pc_ip)
            setText("192.168.1.10") // placeholder; mDNS discovery lands in Phase 1
        }
        toggle = Button(this).apply {
            text = getString(R.string.start)
            setOnClickListener { onToggle() }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 96, 48, 48)
            addView(ipField)
            addView(toggle)
        })
    }

    private fun onToggle() {
        if (streaming) {
            stopStreaming()
        } else if (hasMicPermission()) {
            startStreaming()
        } else {
            requestMic.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private fun startStreaming() {
        val ip = ipField.text.toString().trim()
        if (ip.isEmpty()) {
            toast("Enter the PC IP address")
            return
        }
        val intent = Intent(this, StreamingService::class.java).apply {
            putExtra(StreamingService.EXTRA_IP, ip)
            putExtra(StreamingService.EXTRA_PORT, StreamingService.DEFAULT_PORT)
        }
        ContextCompat.startForegroundService(this, intent)
        streaming = true
        toggle.text = getString(R.string.stop)
    }

    private fun stopStreaming() {
        stopService(Intent(this, StreamingService::class.java))
        streaming = false
        toggle.text = getString(R.string.start)
    }

    private fun hasMicPermission() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
}
