plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.oneios.handoff"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.oneios.handoff"
        // minSdk 26: the Galaxy S25 ships well above this; kept low only so the APK still
        // installs (for inspection, not for running the real commands) on an older test device.
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-preflight"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.2.0")
}
