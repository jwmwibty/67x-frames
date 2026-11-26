package com.devdgames.sixsevenframes

import android.app.Activity
import android.content.res.AssetManager
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.os.SystemClock
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        init {
            System.loadLibrary("native-lib")
        }
    }

    private external fun nativeStart(surface: Surface, assets: AssetManager, halfResolution: Boolean): String
    private external fun nativeDrawFrame(): Boolean
    private external fun nativeLastStatus(): String
    private external fun nativeStop()

    private lateinit var statusView: TextView
    private lateinit var resolutionButton: Button
    private var currentSurface: Surface? = null
    private var halfResolution = true
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
            textSize = 14f
            setPadding(24, 20, 24, 20)
            setBackgroundColor(0xAA101820.toInt())
            text = "67xFrames\nWaiting for Android surface..."
        }

        resolutionButton = Button(this).apply {
            text = "HALF RES — switch to FULL"
            setOnClickListener {
                halfResolution = !halfResolution
                currentSurface?.takeIf { it.isValid }?.let { startRenderer(it) }
            }
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
            resolutionButton,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            )
        )
        setContentView(root)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        // The real pixel size is supplied by surfaceChanged().
    }

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
        val result = nativeStart(surface, assets, halfResolution)
        statusView.text = result
        resolutionButton.text = if (halfResolution) {
            "HALF RES — switch to FULL"
        } else {
            "FULL RES — switch to HALF"
        }

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

                    val now = SystemClock.elapsedRealtime()
                    if (now - lastStatusUpdate >= 1000L) {
                        val latest = nativeLastStatus()
                        runOnUiThread {
                            if (rendering) statusView.text = latest
                        }
                        lastStatusUpdate = now
                    }
                }
            }, "67xFrames-Vulkan").apply {
                priority = Thread.NORM_PRIORITY
                start()
            }
        }
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
