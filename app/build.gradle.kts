plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.vcam.controller"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.vcam.controller"
        minSdk = 29                 // Android 10
        targetSdk = 34              // compatible hasta Android 16
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            // solo arm64 moderno; añade "armeabi-v7a" si necesitas 32-bit
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
        vectorDrawables {
            useSupportLibrary = true
        }
    }

    signingConfigs {
        create("release") {
            // CI: variables de entorno; local: comment y usa debug
            storeFile = file(System.getenv("VCAM_KEYSTORE") ?: "keystore.jks")
            storePassword = System.getenv("VCAM_KS_PASS") ?: "vcam123"
            keyAlias = System.getenv("VCAM_KEY_ALIAS") ?: "vcam"
            keyPassword = System.getenv("VCAM_KEY_PASS") ?: "vcam123"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            applicationIdSuffix = ".debug"
        }
    }

    buildFeatures { viewBinding = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging {
        jniLibs.useLegacyPackaging = false
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    implementation("com.github.topjohnwu.libsu:core:5.2.2")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
}
