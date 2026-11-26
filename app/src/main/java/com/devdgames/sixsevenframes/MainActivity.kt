package com.devdgames.sixsevenframes

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView

class MainActivity : Activity() {
    companion object {
        init {
            System.loadLibrary("native-lib")
        }
    }

    private external fun nativeStatus(): String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val status = try {
            nativeStatus()
        } catch (error: UnsatisfiedLinkError) {
            "67xFrames\nNative library could not load:\n${error.message}"
        }

        val view = TextView(this).apply {
            text = status
            textSize = 18f
            gravity = Gravity.CENTER_VERTICAL
            setPadding(32, 32, 32, 32)
        }
        setContentView(view)
    }
}
