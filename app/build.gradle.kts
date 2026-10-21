plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// -PmayaVulkan=OFF builds the CPU-only variant (the CI workflow falls back to it automatically).
val vulkan = !"OFF".equals(findProperty("mayaVulkan") as String?, ignoreCase = true)

android {
    namespace = "com.maya.assistant"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.maya.assistant"
        minSdk = 30
        targetSdk = 35
        versionCode = 3
        versionName = "1.0.2"

        ndk { abiFilters += "arm64-v8a" }  // Redmi 17 5G is arm64 only: halves build time and APK size

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DMAYA_VULKAN=" + if (vulkan) "ON" else "OFF",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets["main"].jniLibs.srcDir("src/main/cpp/third_party/sherpa-onnx/jniLibs")

    buildTypes {
        release {
            isMinifyEnabled = false
            // Personal build: signed with the auto-generated debug key so the APK installs directly.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    packaging {
        jniLibs {
            // AGP may also collect the imported prebuilt libraries; never fail on the duplicate.
            pickFirsts += setOf("**/libonnxruntime.so", "**/libsherpa-onnx-c-api.so")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

dependencies {
    // tar.bz2 extraction for the downloaded model archives (no AndroidX anywhere: the APK stays small)
    implementation("org.apache.commons:commons-compress:1.27.1")
}
