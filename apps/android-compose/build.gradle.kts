// Root Gradle script. Plugins are declared `apply false` so the version catalog
// stays authoritative and module scripts pick versions up without repetition.
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "2.0.20" apply false
}
