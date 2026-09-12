package com.scribatic.app.engine

import android.app.Service
import android.content.Intent
import android.os.IBinder

/**
 * Foreground service that keeps the audio capture thread alive when the app is
 * backgrounded. It holds the microphone and the ring buffer producer only —
 * inference still happens on [kotlinx.coroutines.Dispatchers.Default].
 */
class TranscriptionService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // TODO: startForeground() with a microphone-type notification, then
        // open the AAudio stream and forward callbacks to Engine.pushAudio().
        return START_STICKY
    }
}
