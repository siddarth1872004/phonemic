package com.phonemic

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.util.AttributeSet
import android.util.TypedValue
import android.view.View
import kotlin.math.cos
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * The Start/Stop control: a 270° ring of LED segments around a round key.
 * While live it reads the mic level itself every frame (the activity's 150 ms
 * poll is too slow for a meter); while connecting a lamp chases round it.
 */
class LevelRing(context: Context, attrs: AttributeSet?) : View(context, attrs) {

    enum class State { IDLE, CONNECTING, LIVE, MUTED }

    var state = State.IDLE
        set(v) {
            if (field == v) return
            field = v
            invalidate()
        }

    var title = ""
        set(v) {
            if (field == v) return
            field = v
            invalidate()
        }

    var subtitle = ""
        set(v) {
            if (field == v) return
            field = v
            invalidate()
        }

    /** Linear peak in [0, 1]; read on every frame while [State.LIVE]. */
    var levelSource: () -> Float = { 0f }

    private val accent = attr(R.attr.pmAccent)
    private val amber = attr(R.attr.pmAmber)
    private val red = attr(R.attr.pmRed)
    private val text = attr(R.attr.pmText)
    private val muted = attr(R.attr.pmMuted)
    private val ledOff = attr(R.attr.pmLedOff)
    private val control = attr(R.attr.pmControl)
    private val edge = attr(R.attr.pmEdge)

    private val tick = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        textAlign = Paint.Align.CENTER
        letterSpacing = 0.18f
    }
    private val subPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE
        textAlign = Paint.Align.CENTER
        letterSpacing = 0.2f
    }

    // Fast attack, slow release, and a held peak, like a console meter.
    private var shown = 0f
    private var hold = 0f
    private var holdAt = 0L

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val side = min(w, dp(300f).toInt())
        setMeasuredDimension(w, (side * 0.92f).toInt())
    }

    override fun onDraw(canvas: Canvas) {
        val side = min(width.toFloat(), height / 0.92f)
        val r = side * 0.46f
        val cx = width / 2f
        val cy = r + dp(4f)
        val now = System.currentTimeMillis()

        val live = state == State.LIVE
        val raw = if (live) levelSource() else 0f
        shown = max(raw, shown * 0.86f)
        val pos = meterPos(shown)
        if (pos >= hold || now - holdAt > 1500) {
            hold = pos
            holdAt = now
        }

        // 270° of segments, open at the bottom.
        val ticks = 44
        val a0 = Math.PI * 0.75
        val sweep = Math.PI * 1.5
        tick.strokeWidth = dp(3.4f)
        val chase = (now % 1600L) / 1600f
        val holdIndex = min((hold * ticks).toInt(), ticks - 1)
        for (i in 0 until ticks) {
            val f = (i + 0.5f) / ticks
            val a = a0 + f * sweep
            val r0 = r * 0.80f
            val r1 = r * if (i % 4 == 0) 1f else 0.94f
            var c = ledOff
            when (state) {
                State.LIVE -> c = when {
                    f <= pos -> if (f > 0.9f) red else if (f > 0.75f) amber else accent
                    hold > 0.02f && i == holdIndex -> text
                    else -> ledOff
                }
                State.CONNECTING -> {
                    var d = f - chase
                    if (d < 0) d += 1f
                    if (d < 0.18f) c = withAlpha(accent, 1f - d / 0.18f)
                }
                State.MUTED -> if (i % 4 == 0) c = withAlpha(red, 0.55f)
                State.IDLE -> Unit
            }
            tick.color = c
            canvas.drawLine(
                cx + cos(a).toFloat() * r0, cy + sin(a).toFloat() * r0,
                cx + cos(a).toFloat() * r1, cy + sin(a).toFloat() * r1, tick,
            )
        }

        // The key in the middle.
        val rd = r * 0.64f
        if (live) {
            fill.color = withAlpha(accent, 0.10f + 0.25f * pos)
            canvas.drawCircle(cx, cy, rd + dp(7f), fill)
        }
        fill.color = 0x55000000
        canvas.drawCircle(cx, cy + dp(3f), rd, fill)
        fill.color = control
        canvas.drawCircle(cx, cy, rd, fill)
        stroke.strokeWidth = dp(1f)
        stroke.color = if (live || state == State.IDLE) withAlpha(accent, 0.7f) else edge
        canvas.drawCircle(cx, cy, rd, stroke)
        stroke.color = edge
        canvas.drawCircle(cx, cy, rd - dp(7f), stroke)

        titlePaint.textSize = rd * 0.30f
        titlePaint.color = when (state) {
            State.LIVE, State.IDLE -> accent
            State.MUTED -> red
            State.CONNECTING -> muted
        }
        canvas.drawText(title, cx, cy + titlePaint.textSize * 0.2f, titlePaint)
        subPaint.textSize = rd * 0.12f
        subPaint.color = muted
        canvas.drawText(subtitle, cx, cy + titlePaint.textSize * 0.2f + subPaint.textSize * 2.2f, subPaint)

        if (state == State.LIVE || state == State.CONNECTING || shown > 0.001f) postInvalidateOnAnimation()
    }

    private fun attr(id: Int): Int {
        val v = TypedValue()
        context.theme.resolveAttribute(id, v, true)
        return v.data
    }

    private fun dp(v: Float) = v * resources.displayMetrics.density

    companion object {
        /** Position on a -60..0 dBFS scale, so normal speech fills a useful range. */
        fun meterPos(lin: Float): Float =
            if (lin <= 0.00001f) 0f else ((20f * log10(lin) + 60f) / 60f).coerceIn(0f, 1f)

        private fun withAlpha(c: Int, a: Float) =
            Color.argb((Color.alpha(c) * a.coerceIn(0f, 1f)).toInt(), Color.red(c), Color.green(c), Color.blue(c))
    }
}
