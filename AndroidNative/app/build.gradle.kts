plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin)
}

android {
    namespace = "br.com.redesurftank.aneawesomeutils"
    compileSdk = 34

    defaultConfig {
        minSdk = 21

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {

    api(libs.okhttp)
    api(libs.appcompat)
    api(libs.sentry.android)
    api(libs.dnsjava)
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/FlashRuntimeExtensions.jar"))
}