package com.maya.assistant

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.view.View
import android.view.animation.LinearInterpolator

/** A single soft orb whose colour follows the assistant's state and which pulses while Maya is active. */
class OrbView(context: Context) : View(context) {
    private val core = Paint(Paint.ANTI_ALIAS_FLAG)
    private val halo = Paint(Paint.ANTI_ALIAS_FLAG)
    private var color = Color.parseColor("#7C9CFF")
    private var phase = 0f
    private val animator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1500
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener {
            phase = it.animatedValue as Float
            invalidate()
        }
    }

    fun setLook(color: Int, pulsing: Boolean) {
        this.color = color
        if (pulsing && !animator.isRunning) animator.start()
        if (!pulsing && animator.isRunning) {
            animator.cancel()
            phase = 0f
        }
        invalidate()
    }

    override fun onDetachedFromWindow() {
        animator.cancel()
        super.onDetachedFromWindow()
    }

    private fun lighten(c: Int): Int {
        val hsv = FloatArray(3)
        Color.colorToHSV(c, hsv)
        hsv[1] *= 0.55f
        hsv[2] = minOf(1f, hsv[2] * 1.15f)
        return Color.HSVToColor(hsv)
    }

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val r = minOf(cx, cy)
        if (r <= 1f) return  // not laid out yet: RadialGradient throws on a radius of 0
        val base = r * 0.52f
        if (animator.isRunning) {
            for (i in 0..1) {
                val t = (phase + i * 0.5f) % 1f
                halo.color = color
                halo.alpha = ((1f - t) * 60).toInt()
                canvas.drawCircle(cx, cy, base + (r - base) * t, halo)
            }
        } else {
            halo.color = color
            halo.alpha = 28
            canvas.drawCircle(cx, cy, base * 1.35f, halo)
        }
        core.shader = RadialGradient(cx - base * 0.25f, cy - base * 0.35f, base * 1.3f, intArrayOf(lighten(color), color), null, Shader.TileMode.CLAMP)
        canvas.drawCircle(cx, cy, base, core)
    }
}
