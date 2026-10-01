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
 * recording does, plus the save that follows: started by Record, updated by
 * Pause and Resume, switched to "Saving…" by Stop, and stopped once the note
 * is saved or saving fails. Staying in the foreground through the save keeps
 * Android from killing the process while it finishes the transcript.
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
            notification(this, if (paused) "Recording paused" else "Recording", step = null),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
        )
        running = true
        // Not sticky: after the process dies there is no recording to resume,
        // and a restarted service would show a notification for nothing.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "recording"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "com.scribatic.app.STOP_RECORDING"
        private const val EXTRA_PAUSED = "paused"

        /** Whether the service is in the foreground, so its notification may be updated. */
        @Volatile private var running = false

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

        /**
         * Switches the notification to "Saving…" once the microphone has
         * closed. Posts it directly rather than restarting the service, which
         * Android may refuse while the app is in the background.
         */
        fun saving(context: Context, step: String) {
            if (!running) return
            context.getSystemService(NotificationManager::class.java)
                .notify(NOTIFICATION_ID, notification(context, "Saving…", step))
        }

        fun hide(context: Context) {
            context.stopService(Intent(context, TranscriptionService::class.java))
        }

        /** [step] is null while recording; while saving it names what is happening. */
        private fun notification(context: Context, title: String, step: String?): Notification {
            val manager = context.getSystemService(NotificationManager::class.java)
            if (manager.getNotificationChannel(CHANNEL_ID) == null) {
                manager.createNotificationChannel(
                    NotificationChannel(CHANNEL_ID, "Recording", NotificationManager.IMPORTANCE_LOW).apply {
                        description = "Shown while Scribatic is recording or saving a recording"
                        setShowBadge(false)
                    },
                )
            }
            val open = PendingIntent.getActivity(
                context, 0,
                Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            val builder = NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_recording)
                .setContentTitle(title)
                .setColor(0xFFEB6C36.toInt())
                .setContentIntent(open)
                .setOngoing(true)
                .setSilent(true)
                .setOnlyAlertOnce(true)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            if (step == null) {
                val stop = PendingIntent.getService(
                    context, 1,
                    Intent(context, TranscriptionService::class.java).setAction(ACTION_STOP),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                )
                builder.setContentText("Transcribed on this phone. Nothing is uploaded.")
                    .addAction(0, "Stop and save", stop)
            } else {
                builder.setContentText(step).setProgress(0, 0, true)
            }
            return builder.build()
        }
    }
}
