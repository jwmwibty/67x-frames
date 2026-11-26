package com.devdgames.sixsevenframes

import android.app.Activity
import android.content.res.AssetManager
import android.graphics.Color
import android.os.Bundle
import android.os.SystemClock
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.util.ArrayDeque
import kotlin.math.ceil

class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        init {
            System.loadLibrary("native-lib")
        }
    }

    private external fun nativeStart(
        surface: Surface,
        assets: AssetManager,
        halfResolution: Boolean,
        lightingMode: Int
    ): String

    private external fun nativeDrawFrame(): Boolean
    private external fun nativeLastStatus(): String
    private external fun nativeStop()

    private data class FrameSample(val timestampNs: Long, val frameTimeNs: Long)

    private lateinit var statusView: TextView
    private lateinit var resolutionButton: Button
    private lateinit var lightingButton: Button
    private var currentSurface: Surface? = null
    private var halfResolution = true
    // 0 = fully off, 1 = partial lighting, 2 = maximum lighting quality.
    private var lightingMode = 0 // Safe startup: OFF. User can cycle to PARTIAL and MAX.

    private val frameSamples = ArrayDeque<FrameSample>()
    private var previousFrameNs = 0L

    @Volatile private var rendering = false
    private var renderThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
            keepScreenOn = true
        }

        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 13f
            setPadding(20, 16, 20, 16)
            setBackgroundColor(0xCC101820.toInt())
            text = "67xFrames\nWaiting for Android surface..."
        }

        resolutionButton = Button(this).apply {
            isAllCaps = false
            textSize = 12f
            setOnClickListener {
                halfResolution = !halfResolution
                currentSurface?.takeIf { it.isValid }?.let { startRenderer(it) }
            }
        }

        lightingButton = Button(this).apply {
            isAllCaps = false
            textSize = 12f
            setOnClickListener {
                lightingMode = (lightingMode + 1) % 3
                currentSurface?.takeIf { it.isValid }?.let { startRenderer(it) }
            }
        }

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(8, 0, 8, 8)
            addView(
                resolutionButton,
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            )
            addView(
                lightingButton,
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            )
        }

        val root = FrameLayout(this)
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        )
        root.addView(
            statusView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP
            )
        )
        root.addView(
            controls,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM
            )
        )
        setContentView(root)
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int
    ) {
        currentSurface = holder.surface
        if (!holder.surface.isValid || width <= 0 || height <= 0) {
            statusView.text = "67xFrames\nWaiting for a valid surface..."
            return
        }
        startRenderer(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        currentSurface = null
        stopRenderer()
    }

    private fun startRenderer(surface: Surface) {
        stopRenderer()
        resetFrameStats()

        resolutionButton.text = if (halfResolution) "HALF RES" else "FULL RES"
        lightingButton.text = when (lightingMode) {
            0 -> "LIGHTS: OFF"
            1 -> "LIGHTS: PARTIAL"
            else -> "LIGHTS: MAX"
        }

        val result = nativeStart(surface, assets, halfResolution, lightingMode)
        statusView.text = result

        if (result.startsWith("READY")) {
            rendering = true
            renderThread = Thread({
                var lastStatusUpdate = SystemClock.elapsedRealtime()
                while (rendering) {
                    if (!nativeDrawFrame()) {
                        rendering = false
                        val error = nativeLastStatus()
                        runOnUiThread { statusView.text = error }
                        break
                    }

                    val nowNs = SystemClock.elapsedRealtimeNanos()
                    recordFrame(nowNs)

                    val nowMs = SystemClock.elapsedRealtime()
                    if (nowMs - lastStatusUpdate >= 1000L) {
                        val latest = buildPerformanceReport(nativeLastStatus())
                        runOnUiThread {
                            if (rendering) statusView.text = latest
                        }
                        lastStatusUpdate = nowMs
                    }
                }
            }, "67xFrames-Vulkan").apply {
                priority = Thread.NORM_PRIORITY
                start()
            }
        }
    }

    private fun resetFrameStats() {
        frameSamples.clear()
        previousFrameNs = 0L
    }

    private fun recordFrame(nowNs: Long) {
        if (previousFrameNs != 0L) {
            val frameTime = nowNs - previousFrameNs
            if (frameTime > 0L) {
                frameSamples.addLast(FrameSample(nowNs, frameTime))
            }
        }
        previousFrameNs = nowNs

        val cutoff = nowNs - 10_000_000_000L
        while (frameSamples.peekFirst()?.timestampNs?.let { it < cutoff } == true) {
            frameSamples.removeFirst()
        }
    }

    private fun buildPerformanceReport(rendererStatus: String): String {
        if (frameSamples.isEmpty()) {
            return "$rendererStatus\nFPS average (10s): measuring...\n1% low: measuring..."
        }

        val frameTimes = frameSamples.map { it.frameTimeNs }
        val totalTimeNs = frameTimes.sumOf { it.toDouble() }
        val averageFps = frameTimes.size.toDouble() * 1_000_000_000.0 / totalTimeNs

        val slowFrameCount = maxOf(1, ceil(frameTimes.size * 0.01).toInt())
        val slowestAverageNs = frameTimes.sortedDescending()
            .take(slowFrameCount)
            .average()
        val onePercentLowFps = 1_000_000_000.0 / slowestAverageNs
        val averageFrameMs = totalTimeNs / frameTimes.size / 1_000_000.0

        return rendererStatus +
            "\nFPS average (10s): %.1f".format(averageFps) +
            "\n1%% low: %.1f FPS".format(onePercentLowFps) +
            "\nAverage frame: %.2f ms".format(averageFrameMs)
    }

    private fun stopRenderer() {
        rendering = false
        val thread = renderThread
        renderThread = null
        if (thread != null && thread !== Thread.currentThread()) {
            try {
                thread.join()
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        nativeStop()
    }

    override fun onDestroy() {
        stopRenderer()
        super.onDestroy()
    }
}
