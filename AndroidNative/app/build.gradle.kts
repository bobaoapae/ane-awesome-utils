plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin)
}

android {
    namespace = "br.com.redesurftank.aneawesomeutils"
    compileSdk = 36
    ndkVersion= "28.1.13356709"

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
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.0.2"
        }
    }
}

dependencies {

    api(libs.okhttp)
    api(libs.appcompat)
    api(libs.sentry.android)
    api(libs.dnsjava)
    api(libs.fastXml)
    api(libs.staxApi)
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/FlashRuntimeExtensions.jar"))
}