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

        // shadowhook only ships arm64-v8a + armeabi-v7a prefab libs.
        // Android Adobe AIR PVP target is ARM-only anyway (no x86 phones in
        // production for this game), so restricting ABIs is safe.
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
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
    buildFeatures {
        prefab = true
    }
}

dependencies {

    api(libs.okhttp)
    api(libs.appcompat)
    api(libs.dnsjava)
    api(libs.fastXml)
    api(libs.staxApi)
    // shadowhook — ByteDance inline hook library, used to instrument
    // libCore.so malloc/free/mmap/munmap for native allocation tracing.
    implementation("com.bytedance.android:shadowhook:2.0.0")
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/FlashRuntimeExtensions.jar"))
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/lib/runtimeClasses.jar"))
}