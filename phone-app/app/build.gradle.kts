plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.phonemic"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.phonemic"
        minSdk = 29          // Android 10: reliable low-latency AAudio via Oboe
        targetSdk = 34
        versionCode = 2
        versionName = "0.2.0"

        // Only ship 64-bit ABIs; drop this list to add 32-bit if ever needed.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // C++17, and tell CMake we build a shared lib for JNI.
                cppFlags += "-std=c++17"
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

    // Oboe ships as a Prefab-enabled AAR; enable Prefab so CMake can find it.
    buildFeatures {
        prefab = true
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
            isMinifyEnabled = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    // Material 3 components for the UI (cards, switches, buttons, meter).
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")

    // Oboe: low-latency audio (AAudio/OpenSL ES). The audio hot path lives in
    // C++ against this; Kotlin never touches an audio buffer (principle #1).
    implementation("com.google.oboe:oboe:1.9.0")
}
