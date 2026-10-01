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
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.scribatic.app"
        minSdk        = 28          // AAudio low-latency callback + mmap headroom
        targetSdk     = 36
        versionCode   = 7
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
                    "-DCMAKE_BUILD_TYPE=Release",
                    // 16 KB ELF alignment for every shared library, the vendored
                    // whisper/ggml/llama ones included — Play requires it for
                    // Android 15+ targets. NDK r28+ does this by default.
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
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

    // Models ship as Play asset packs, not inside the APK (ADR-011). Only a
    // bundle built for Play carries them; a debug APK has none and falls back
    // to importing the files on the setup screen.
    // -Pscribatic.skipAnswersPack=true leaves the 1.2 GB instruct pack out,
    // for local bundletool testing on a machine or emulator short of space.
    assetPacks += listOf(":models-core")
    if (project.findProperty("scribatic.skipAnswersPack") != "true") {
        assetPacks += listOf(":models-answers")
    }

    // Speaker diarization (ADR-009): sherpa-onnx's C API and ONNX Runtime,
    // prebuilt and staged by `make fetch-deps`. libscribatic_engine.so links
    // against them, so they ship alongside it. Absent, the engine is built
    // without diarization and nothing here is packaged.
    sourceSets {
        getByName("main") {
            jniLibs.srcDir(rootProject.file("../../core/engine/vendor-bin/sherpa-onnx/android"))
        }
    }
}

dependencies {
    // Asks the Play Store for the model packs. The Store downloads them; the
    // app itself still has no INTERNET permission and makes no requests.
    implementation("com.google.android.play:asset-delivery-ktx:2.3.0")
    implementation(platform("androidx.compose:compose-bom:2024.09.03"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    // Rounded icons for every control. R8 strips the ones not referenced.
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
