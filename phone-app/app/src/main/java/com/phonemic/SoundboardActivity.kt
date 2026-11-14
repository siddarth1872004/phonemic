package com.phonemic

import android.annotation.SuppressLint
import android.app.Activity
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.TypedValue
import android.view.View
import android.webkit.CookieManager
import android.webkit.RenderProcessGoneDetail
import android.webkit.WebChromeClient
import android.webkit.WebMessage
import android.webkit.WebMessagePort
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ProgressBar
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.URL
import java.nio.ByteOrder
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.HttpsURLConnection

/**
 * The real MyInstants site, with its sound buttons sent into the mic stream
 * instead of the phone's speaker.
 *
 * The site plays every sound with HTMLMediaElement.play(). An injected script
 * replaces play() so nothing reaches the speaker and passes the clip's URL back
 * over a WebMessage port posted only to the MyInstants origin. That avoids
 * addJavascriptInterface, which every frame on the page (ads included) could
 * call. The clip is downloaded and decoded here, then queued into the native
 * engine, which mixes it with the mic before Opus. Streaming itself stays in
 * StreamingService, so this screen can come and go freely.
 */
class SoundboardActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    // Bumped by every press and by Stop; a decode whose number is stale gives up.
    private val playGen = AtomicInteger()
    private val cache = object : LinkedHashMap<String, ByteArray>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, ByteArray>) = size > CACHE_CLIPS
    }  // worker thread only

    private lateinit var frame: FrameLayout
    private lateinit var blocker: View
    private lateinit var errorPanel: View
    private lateinit var errorText: TextView
    private lateinit var progress: ProgressBar
    private lateinit var dot: View
    private lateinit var status: TextView
    private lateinit var stop: Button

    private var web: WebView? = null
    private var port: WebMessagePort? = null
    private var pageGen = 0
    private var hookedGen = -1
    private var userAgent = ""
    @Volatile private var playing: String? = null
    @Volatile private var failedAt = 0L

    private val poll = object : Runnable {
        override fun run() {
            renderStatus()
            handler.postDelayed(this, 300)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        MainActivity.applyAppearance(this)
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_soundboard)

        frame = findViewById(R.id.sbWebFrame)
        blocker = findViewById(R.id.sbBlocker)
        errorPanel = findViewById(R.id.sbError)
        errorText = findViewById(R.id.sbErrorText)
        progress = findViewById(R.id.sbProgress)
        dot = findViewById(R.id.sbDot)
        status = findViewById(R.id.sbStatus)
        stop = findViewById(R.id.sbStop)
        stop.clipToOutline = true
        stop.setOnClickListener { stopSound() }
        findViewById<Button>(R.id.sbRetry).apply {
            clipToOutline = true
            setOnClickListener {
                errorPanel.visibility = View.GONE
                web?.reload() ?: createWebView(null)
            }
        }

        val gainText = findViewById<TextView>(R.id.sbGainValue)
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        findViewById<SeekBar>(R.id.sbGain).apply {
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    NativeBridge.setAuxGain(value / 100f)
                    gainText.text = getString(R.string.sb_level_fmt, value)
                    if (fromUser) prefs.edit().putInt(PREF_GAIN, value).apply()
                }
                override fun onStartTrackingTouch(bar: SeekBar) = Unit
                override fun onStopTrackingTouch(bar: SeekBar) = Unit
            })
            progress = prefs.getInt(PREF_GAIN, DEFAULT_GAIN_PERCENT).coerceIn(0, max)
            NativeBridge.setAuxGain(progress / 100f)
            gainText.text = getString(R.string.sb_level_fmt, progress)
        }

        userAgent = WebSettings.getDefaultUserAgent(this)
        createWebView(savedInstanceState)
    }

    override fun onResume() {
        super.onResume()
        web?.onResume()
        handler.post(poll)
    }

    override fun onPause() {
        handler.removeCallbacks(poll)
        web?.onPause()
        super.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        web?.saveState(outState)
    }

    override fun onDestroy() {
        stopSound()
        worker.shutdownNow()
        closePort()
        web?.let {
            frame.removeView(it)
            it.destroy()
        }
        web = null
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    @Suppress("OVERRIDE_DEPRECATION")
    override fun onBackPressed() {
        val w = web
        if (w != null && w.canGoBack()) w.goBack() else super.onBackPressed()
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun createWebView(state: Bundle?) {
        val w = WebView(this)
        w.setBackgroundColor(color(R.attr.pmBg))
        w.settings.apply {
            javaScriptEnabled = true  // the site's sound buttons are script
            domStorageEnabled = true
            allowFileAccess = false
            allowContentAccess = false
            setGeolocationEnabled(false)
            setSupportMultipleWindows(false)  // window.open stays here, through the navigation filter
            javaScriptCanOpenWindowsAutomatically = false
            mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
        }
        w.webViewClient = Client()
        w.webChromeClient = object : WebChromeClient() {
            override fun onProgressChanged(view: WebView, newProgress: Int) {
                if (view !== web) return
                progress.progress = newProgress
                progress.visibility = if (newProgress < 100) View.VISIBLE else View.INVISIBLE
            }
        }
        frame.addView(w, 0, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        web = w
        blocker.visibility = View.VISIBLE
        if (state == null || w.restoreState(state) == null) w.loadUrl(HOME)
    }

    private inner class Client : WebViewClient() {
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean =
            request.isForMainFrame && !isMyInstants(request.url)

        override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
            if (view !== web) return
            // A new document has no hook yet: hold taps off until it does.
            pageGen++
            closePort()
            blocker.visibility = View.VISIBLE
            errorPanel.visibility = View.GONE
        }

        override fun onPageCommitVisible(view: WebView, url: String) = hook(view, url)

        override fun onPageFinished(view: WebView, url: String) = hook(view, url)

        override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
            if (view === web && request.isForMainFrame) showError(getString(R.string.sb_page_failed))
        }

        override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
            Log.w(TAG, "WebView renderer gone (crashed: ${detail.didCrash()})")
            if (view === web) {
                web = null
                closePort()
                showError(getString(R.string.sb_crashed))
            }
            frame.removeView(view)
            view.destroy()
            return true  // handled: the app keeps running, Retry builds a new WebView
        }
    }

    /** Install the play() hook in the current page and hand it the port. */
    private fun hook(view: WebView, url: String) {
        if (view !== web || hookedGen == pageGen) return
        val uri = Uri.parse(url)
        if (!isMyInstants(uri)) return
        val gen = pageGen
        hookedGen = gen
        val nonce = UUID.randomUUID().toString()
        view.evaluateJavascript(hookScript(nonce)) { result ->
            if (view !== web || gen != pageGen) return@evaluateJavascript
            if (result != "\"ok\"") {
                Log.w(TAG, "hook not installed: $result")
                hookedGen = -1  // try again at onPageFinished
                return@evaluateJavascript
            }
            val ports = view.createWebMessageChannel()
            ports[0].setWebMessageCallback(object : WebMessagePort.WebMessageCallback() {
                override fun onMessage(p: WebMessagePort, message: WebMessage) = onSoundPressed(message.data)
            })
            closePort()
            port = ports[0]
            // Delivered to the main frame only, and only while it is MyInstants.
            view.postWebMessage(WebMessage(nonce, arrayOf(ports[1])), Uri.parse("https://${uri.host}"))
            blocker.visibility = View.GONE
        }
    }

    private fun closePort() {
        port?.close()
        port = null
    }

    private fun onSoundPressed(raw: String?) {
        val uri = raw?.let(Uri::parse) ?: return
        if (!isSoundUrl(uri)) {
            Log.w(TAG, "ignored non-sound URL from the page")
            return
        }
        if (!NativeBridge.isRunning()) {
            Toast.makeText(this, R.string.sb_not_streaming, Toast.LENGTH_SHORT).show()
            return
        }
        val gen = playGen.incrementAndGet()
        val name = uri.lastPathSegment.orEmpty().removeSuffix(".mp3")
        playing = name
        renderStatus()
        val url = uri.toString()
        val cookies = CookieManager.getInstance().getCookie(url)
        worker.execute { play(gen, url, cookies) }
    }

    private fun stopSound() {
        playGen.incrementAndGet()
        NativeBridge.auxStop()
        playing = null
    }

    // ---- worker thread ----

    private fun play(gen: Int, url: String, cookies: String?) {
        try {
            val data = cache[url] ?: download(url, cookies).also { cache[url] = it }
            if (gen == playGen.get()) decode(gen, data)
        } catch (e: IOException) {
            fail(gen, url, e)
        } catch (e: IllegalStateException) {  // MediaCodec / MediaExtractor
            fail(gen, url, e)
        } catch (e: IllegalArgumentException) {
            fail(gen, url, e)
        } finally {
            if (gen == playGen.get()) playing = null
        }
    }

    private fun fail(gen: Int, url: String, e: Exception) {
        Log.w(TAG, "sound $url failed: $e")
        if (gen == playGen.get()) failedAt = SystemClock.uptimeMillis()
    }

    private fun download(url: String, cookies: String?): ByteArray {
        val c = URL(url).openConnection() as HttpsURLConnection
        try {
            c.instanceFollowRedirects = false  // a redirect could lead off MyInstants
            c.connectTimeout = 8000
            c.readTimeout = 8000
            c.setRequestProperty("User-Agent", userAgent)
            if (!cookies.isNullOrEmpty()) c.setRequestProperty("Cookie", cookies)
            if (c.responseCode != 200) throw IOException("HTTP ${c.responseCode}")
            if (c.contentLengthLong > MAX_CLIP_BYTES) throw IOException("clip too large")
            return c.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                val buf = ByteArray(16 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    if (out.size() > MAX_CLIP_BYTES) throw IOException("clip too large")
                }
                out.toByteArray()
            }
        } finally {
            c.disconnect()
        }
    }

    /** Decode to PCM and queue it as it comes, paced by the engine's bounded aux queue. */
    private fun decode(gen: Int, data: ByteArray) {
        val ex = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            ex.setDataSource(ByteSource(data))
            val track = (0 until ex.trackCount).firstOrNull {
                ex.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: throw IOException("no audio track")
            ex.selectTrack(track)
            val format = ex.getTrackFormat(track)
            val c = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec = c
            c.configure(format, null, null, 0)
            c.start()

            val info = MediaCodec.BufferInfo()
            var inputDone = false
            var channels = 0
            var isFloat = false
            var pcm = ShortArray(0)
            while (gen == playGen.get()) {
                if (!inputDone) {
                    val i = c.dequeueInputBuffer(10_000)
                    if (i >= 0) {
                        val n = ex.readSampleData(c.getInputBuffer(i)!!, 0)
                        if (n < 0) {
                            c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            c.queueInputBuffer(i, 0, n, ex.sampleTime, 0)
                            ex.advance()
                        }
                    }
                }
                val o = c.dequeueOutputBuffer(info, 10_000)
                if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED || (o >= 0 && channels == 0)) {
                    val f = c.outputFormat
                    channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    isFloat = f.containsKey(MediaFormat.KEY_PCM_ENCODING) &&
                        f.getInteger(MediaFormat.KEY_PCM_ENCODING) == AudioFormat.ENCODING_PCM_FLOAT
                    if (!NativeBridge.auxBegin(f.getInteger(MediaFormat.KEY_SAMPLE_RATE), channels)) {
                        throw IOException("unsupported format $f")
                    }
                }
                if (o < 0) continue
                val buf = c.getOutputBuffer(o)!!.order(ByteOrder.nativeOrder())
                buf.position(info.offset)
                buf.limit(info.offset + info.size)
                val samples = info.size / if (isFloat) 4 else 2
                if (pcm.size < samples) pcm = ShortArray(samples)
                if (isFloat) {
                    val fb = buf.asFloatBuffer()
                    for (k in 0 until samples) pcm[k] = (fb.get(k).coerceIn(-1f, 1f) * 32767f).toInt().toShort()
                } else {
                    buf.asShortBuffer().get(pcm, 0, samples)
                }
                c.releaseOutputBuffer(o, false)
                if (!feed(gen, pcm, samples / channels, channels)) return
                if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return
            }
        } finally {
            codec?.release()
            ex.release()
        }
    }

    /** False when this clip was replaced or stopped, or streaming ended. */
    private fun feed(gen: Int, pcm: ShortArray, frames: Int, channels: Int): Boolean {
        var off = 0
        while (off < frames) {
            if (gen != playGen.get()) return false
            val n = NativeBridge.auxWrite(pcm, off, frames - off, channels)
            if (n < 0) return false
            off += n
            if (off < frames) SystemClock.sleep(5)  // queue full: wait for the sender to drain a frame
        }
        return true
    }

    private class ByteSource(private val data: ByteArray) : MediaDataSource() {
        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
            if (position >= data.size) return -1
            val n = minOf(size.toLong(), data.size - position).toInt()
            System.arraycopy(data, position.toInt(), buffer, offset, n)
            return n
        }
        override fun getSize() = data.size.toLong()
        override fun close() = Unit
    }

    // ---- UI ----

    private fun renderStatus() {
        val running = NativeBridge.isRunning()
        val p = playing
        val (text, tone) = when {
            !running -> getString(R.string.sb_not_streaming) to R.attr.pmAmber
            StreamingService.muted -> getString(R.string.sb_muted) to R.attr.pmAmber
            failedAt != 0L && SystemClock.uptimeMillis() - failedAt < 3000 -> getString(R.string.sb_sound_failed) to R.attr.pmRed
            p != null -> getString(R.string.sb_playing_fmt, p) to R.attr.pmGreen
            else -> getString(R.string.sb_ready) to R.attr.pmGreen
        }
        if (status.text.toString() != text) status.text = text
        dot.backgroundTintList = ColorStateList.valueOf(color(tone))
        stop.visibility = if (running && p != null) View.VISIBLE else View.GONE
    }

    private fun showError(msg: String) {
        errorText.text = msg
        errorPanel.visibility = View.VISIBLE
        progress.visibility = View.INVISIBLE
    }

    private fun color(attr: Int): Int {
        val v = TypedValue()
        theme.resolveAttribute(attr, v, true)
        return v.data
    }

    companion object {
        private const val TAG = "phonemic"
        private const val PREFS = "phonemic"
        private const val PREF_GAIN = "soundboard_gain"
        private const val DEFAULT_GAIN_PERCENT = 50  // AuxMixer::kDefaultGain
        private const val HOME = "https://www.myinstants.com/en/index/us/"
        private const val MAX_CLIP_BYTES = 4 * 1024 * 1024
        private const val CACHE_CLIPS = 16

        private fun isMyInstants(uri: Uri): Boolean {
            val host = uri.host ?: return false
            return uri.scheme == "https" && uri.port == -1 && uri.userInfo == null &&
                (host == "myinstants.com" || host.endsWith(".myinstants.com"))
        }

        private fun isSoundUrl(uri: Uri): Boolean {
            val path = uri.path ?: return false
            return isMyInstants(uri) && path.startsWith("/media/sounds/") && !path.contains("..")
        }

        /**
         * Runs in the page (main frame). play() never plays: it forwards a
         * MyInstants sound URL to the port and reports success, so the site's
         * own button state stays normal. Anything that starts on its own
         * (autoplay) is paused, so the speaker stays silent.
         */
        private fun hookScript(nonce: String) = """
            (function () {
              var w = window;
              w.__pmNonce = '$nonce';
              if (w.__pmHooked) return 'ok';
              w.__pmHooked = true;
              var port = null;
              w.addEventListener('message', function (e) {
                if (e.data === w.__pmNonce && e.ports && e.ports.length === 1) {
                  port = e.ports[0];
                  e.stopImmediatePropagation();
                }
              }, true);
              function soundUrl(el) {
                var src = el.src || el.currentSrc;
                if (!src && el.querySelector) {
                  var s = el.querySelector('source[src]');
                  if (s) src = s.src;
                }
                try {
                  var u = new URL(src, location.href);
                  if (u.protocol === 'https:' && /(^|\.)myinstants\.com${'$'}/.test(u.hostname) &&
                      u.pathname.indexOf('/media/sounds/') === 0) return u.href;
                } catch (err) {}
                return null;
              }
              HTMLMediaElement.prototype.play = function () {
                var u = soundUrl(this);
                if (u && port) port.postMessage(u);
                return Promise.resolve();
              };
              document.addEventListener('play', function (e) {
                if (e.target && e.target.pause) { e.target.muted = true; e.target.pause(); }
              }, true);
              return 'ok';
            })();
        """.trimIndent()
    }
}
