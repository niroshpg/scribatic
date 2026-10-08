package com.scribatic.app.engine

import android.app.ActivityManager
import android.content.Context
import android.os.StatFs
import java.io.File

/**
 * Free storage, what Scribatic itself keeps, and memory: for the Storage &
 * memory panel, and the checks before and during a recording.
 *
 * A recording is stored as 16 kHz mono float32, what the speaker step reads in
 * place, so it takes 64 kB a second: 230 MB an hour. That is the number every
 * estimate here is built on.
 */
data class DeviceStatus(
    val freeBytes: Long,
    val recordingsBytes: Long,
    val recordingCount: Int,
    val modelsBytes: Long,
    val notesBytes: Long,
    /** Memory the system could hand out now, and the device's total. */
    val availableMemory: Long,
    val totalMemory: Long,
    /** The system considers memory low and may start closing apps. */
    val lowMemory: Boolean,
) {
    /** Roughly how much can still be recorded before the minimum is reached. */
    val recordingSecondsLeft: Long get() = maxOf(0L, freeBytes - MINIMUM_FREE) / BYTES_PER_SECOND

    companion object {
        const val BYTES_PER_SECOND = 64_000L

        /**
         * Below this a recording does not start, and one in progress is stopped
         * and saved before writes begin to fail.
         */
        const val MINIMUM_FREE = 50_000_000L

        /** Choices for the warning before recording; 1 GB is about 4 hours. */
        val WARNING_LEVELS = listOf(500_000_000L, 1_000_000_000L, 2_000_000_000L)

        private const val PREFS = "settings"
        private const val WARNING_KEY = "storageWarningBytes"

        fun warningLevel(context: Context): Long =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getLong(WARNING_KEY, 1_000_000_000L)
                .takeIf { it in WARNING_LEVELS } ?: 1_000_000_000L

        fun setWarningLevel(context: Context, bytes: Long) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putLong(WARNING_KEY, bytes).apply()
        }

        fun freeBytes(context: Context): Long = StatFs(context.filesDir.absolutePath).availableBytes

        fun memory(context: Context): ActivityManager.MemoryInfo =
            ActivityManager.MemoryInfo().also {
                (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(it)
            }

        /** [models] are the model files in use, wherever they were installed. */
        fun current(context: Context, recordingsDir: File, models: List<File>, storeDir: File): DeviceStatus {
            val recordings = recordingsDir.listFiles().orEmpty()
            val memory = memory(context)
            return DeviceStatus(
                freeBytes = freeBytes(context),
                recordingsBytes = recordings.sumOf { it.length() },
                recordingCount = recordings.count { it.extension == "wav" || it.extension == "m4a" },
                modelsBytes = models.filter { it.exists() }.sumOf { it.length() },
                notesBytes = storeDir.listFiles().orEmpty().sumOf { it.length() },
                availableMemory = memory.availMem,
                totalMemory = memory.totalMem,
                lowMemory = memory.lowMemory,
            )
        }
    }
}
