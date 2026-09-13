// Root Gradle script. Plugins are declared `apply false` so the version catalog
// stays authoritative and module scripts pick versions up without repetition.
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "2.0.20" apply false
    // Required from Kotlin 2.0 on: the Compose compiler ships with Kotlin and
    // is configured by this plugin rather than by composeOptions.
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.20" apply false
}
