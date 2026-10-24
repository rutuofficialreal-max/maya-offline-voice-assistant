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
        versionCode = 6
        versionName = "1.1.0"

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

    // A stable release key, so v1.1.1 installs *over* v1.1.0 instead of forcing an uninstall.
    // The keystore is never in the repo: CI decodes it from repository secrets into a temp file and
    // points MAYA_KEYSTORE at it. Without those secrets (local builds, forks) the debug key is used.
    signingConfigs {
        create("mayaRelease") {
            val ks = System.getenv("MAYA_KEYSTORE") ?: (findProperty("mayaKeystore") as String?)
            if (ks != null && file(ks).exists()) {
                storeFile = file(ks)
                storePassword = System.getenv("MAYA_KEYSTORE_PASSWORD") ?: (findProperty("mayaKeystorePassword") as String?)
                keyAlias = System.getenv("MAYA_KEY_ALIAS") ?: (findProperty("mayaKeyAlias") as String?) ?: "maya"
                keyPassword = System.getenv("MAYA_KEY_PASSWORD") ?: (findProperty("mayaKeyPassword") as String?) ?: storePassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            val release = signingConfigs.getByName("mayaRelease")
            // Only use it when the key *and* its password are actually present, otherwise Gradle fails
            // with "missing required property storePassword" instead of falling back.
            val haveKey = System.getenv("MAYA_KEYSTORE")?.let { file(it).exists() } == true &&
                !System.getenv("MAYA_KEYSTORE_PASSWORD").isNullOrEmpty()
            // Signed with the stable key when CI provides it, otherwise the debug key so local builds still install.
            signingConfig = if (haveKey) release else signingConfigs.getByName("debug")
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
