package com.devdgames.sixsevenframes

import android.app.Activity
import android.content.res.AssetManager
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        init {
            System.loadLibrary("native-lib")
        }
    }

    private external fun nativeStart(surface: Surface, assets: AssetManager): String
    private external fun nativeDrawFrame(): Boolean
    private external fun nativeLastStatus(): String
    private external fun nativeStop()

    private lateinit var statusView: TextView
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
        stopRenderer()

        if (!holder.surface.isValid || width <= 0 || height <= 0) {
            statusView.text = "67xFrames\nWaiting for a valid surface..."
            return
        }

        val result = nativeStart(holder.surface, assets)
        statusView.text = result

        if (result.startsWith("READY")) {
            rendering = true
            renderThread = Thread({
                while (rendering) {
                    if (!nativeDrawFrame()) {
                        rendering = false
                        val error = nativeLastStatus()
                        runOnUiThread { statusView.text = error }
                        break
                    }
                }
            }, "67xFrames-Vulkan").apply {
                priority = Thread.NORM_PRIORITY
                start()
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopRenderer()
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
