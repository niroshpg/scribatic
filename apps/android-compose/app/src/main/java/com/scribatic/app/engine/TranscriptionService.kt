package com.scribatic.app.engine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.scribatic.app.R
import com.scribatic.app.ui.MainActivity
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow

/**
 * Keeps a recording going while the app is not on screen.
 *
 * Capture itself stays in the view model's AudioRecord thread. This service
 * exists because Android only lets an app keep using the microphone in the
 * background from a foreground service of the microphone type, and that
 * service must show a notification — which is also how the user can see,
 * from anywhere, that the microphone is open. It runs exactly as long as a
 * recording does: started by Record, updated by Pause and Resume, stopped by
 * Stop or a failure.
 */
class TranscriptionService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            // From the notification: the view model saves the note, then
            // stops this service like any other stop.
            stopRequests.tryEmit(Unit)
            return START_NOT_STICKY
        }
        val paused = intent?.getBooleanExtra(EXTRA_PAUSED, false) ?: false
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            notification(paused),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
        )
        // Not sticky: after the process dies there is no recording to resume,
        // and a restarted service would show a notification for nothing.
        return START_NOT_STICKY
    }

    private fun notification(paused: Boolean): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Recording", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Shown while Scribatic is recording"
                    setShowBadge(false)
                },
            )
        }
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, TranscriptionService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_recording)
            .setContentTitle(if (paused) "Recording paused" else "Recording")
            .setContentText("Transcribed on this phone. Nothing is uploaded.")
            .setColor(0xFFEB6C36.toInt())
            .setContentIntent(open)
            .addAction(0, "Stop and save", stop)
            .setOngoing(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "recording"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "com.scribatic.app.STOP_RECORDING"
        private const val EXTRA_PAUSED = "paused"

        private val stopRequests = MutableSharedFlow<Unit>(
            extraBufferCapacity = 1,
            onBufferOverflow = BufferOverflow.DROP_OLDEST,
        )

        /** Stop presses from the notification, for the view model to act on. */
        val stops: SharedFlow<Unit> = stopRequests

        /**
         * Starts the service, or updates its notification if it is running.
         * Must first be called while the app is on screen: Android refuses to
         * start a microphone foreground service from the background.
         */
        fun show(context: Context, paused: Boolean) {
            val intent = Intent(context, TranscriptionService::class.java).putExtra(EXTRA_PAUSED, paused)
            ContextCompat.startForegroundService(context, intent)
        }

        fun hide(context: Context) {
            context.stopService(Intent(context, TranscriptionService::class.java))
        }
    }
}
