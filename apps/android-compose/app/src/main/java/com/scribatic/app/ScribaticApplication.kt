package com.scribatic.app

import android.app.Application
import com.scribatic.app.ext.Extensions

/**
 * Application entry point. Deliberately does no engine work: model residency is
 * established lazily on first use so cold start is not charged for an mmap.
 */
class ScribaticApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        Extensions.install(this)
    }
}
