plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.phonemic"
    compileSdk = 34
    // Pinned so every machine/CI builds the native code with the same toolchain.
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.phonemic"
        minSdk = 29          // Android 10: reliable low-latency AAudio via Oboe
        targetSdk = 34
        versionCode = 4
        versionName = "0.3.1"

        // 64-bit ABIs (all current phones) plus x86_64 for the emulator.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // Oboe's prefab package is built against the shared C++ runtime,
                // so we must match it (the NDK default is c++_static).
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = true       // Oboe ships as a Prefab AAR
        buildConfig = true  // BuildConfig.VERSION_NAME for the UI
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Signed with the debug key unless you configure your own release key,
            // so `assembleRelease` produces an installable APK out of the box.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    lint {
        abortOnError = true
        warningsAsErrors = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    // Material 3 components for the UI (cards, switches, buttons, meter).
    implementation("com.google.android.material:material:1.12.0")

    // Oboe: low-latency audio (AAudio/OpenSL ES). The audio hot path lives in
    // C++ against this; Kotlin never touches an audio buffer (principle #1).
    implementation("com.google.oboe:oboe:1.9.0")
}
