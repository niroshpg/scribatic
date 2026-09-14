import java.io.FileInputStream
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release signing is read from keystore.properties in apps/android-compose/,
// which is gitignored. When it is absent — fresh clone, CI, debug-only work —
// the release signingConfig is left unconfigured and the bundle comes out
// unsigned instead of the build failing on a missing secret.
val keystoreProperties = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) FileInputStream(f).use { load(it) }
}
val hasReleaseKeystore = keystoreProperties.getProperty("storeFile") != null

android {
    namespace  = "com.scribatic.app"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.scribatic.app"
        minSdk        = 28          // AAudio low-latency callback + mmap headroom
        targetSdk     = 35
        versionCode   = 1
        versionName   = "0.1.0"

        ndk {
            // Single ABI: see scribatic.abiFilters in gradle.properties.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                // -O3 and the NEON flags are applied inside CMakeLists.txt so
                // the same optimisation profile is used when the core is built
                // standalone for CI or for the iOS target.
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_ARM_NEON=ON",
                    "-DCMAKE_BUILD_TYPE=Release"
                )
                cppFlags += "-std=c++17"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path    = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        create("release") {
            if (hasReleaseKeystore) {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            if (hasReleaseKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
            isMinifyEnabled   = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Keep unstripped .so files for symbolicating native crashes.
            ndk { debugSymbolLevel = "FULL" }
        }
        debug {
            isJniDebuggable = true
        }
    }

    // GGUF weights are streamed into filesDir on first run rather than bundled,
    // so the Play asset cap is never a constraint and the mmap target is a real
    // file rather than a compressed asset entry.
    androidResources {
        noCompress += listOf("gguf", "bin")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose  = true
        buildConfig = true
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false   // uncompressed .so, loaded via mmap
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.09.03"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
