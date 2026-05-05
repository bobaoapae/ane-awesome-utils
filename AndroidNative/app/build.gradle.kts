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

// `embedded` — JARs that are merged INTO the AAR's classes.jar at bundle
// time (instead of being declared as transitive deps). Used for dnsjava
// because we ship a verifier-friendly patched AsyncSemaphore (see
// src/main/java/org/xbill/DNS/AsyncSemaphore.java) and need the rest of
// dnsjava co-located so R8 doesn't see two copies of any class.
val embedded by configurations.creating {
    isCanBeResolved = true
    isCanBeConsumed = false
    isTransitive = false
}

dependencies {

    api(libs.okhttp)
    api(libs.appcompat)
    // dnsjava: compile-only at the ANE level so its classes are visible
    // to our InternalDnsResolver, but it's NOT exposed to consumers as
    // a transitive dependency. The `embedded` config below pulls the
    // same artifact, and we merge it into classes.jar at bundle time.
    compileOnly(libs.dnsjava)
    embedded(libs.dnsjava)
    api(libs.fastXml)
    api(libs.staxApi)
    // shadowhook — ByteDance inline hook library, used to instrument
    // libCore.so malloc/free/mmap/munmap for native allocation tracing.
    implementation("com.bytedance.android:shadowhook:2.0.0")
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/FlashRuntimeExtensions.jar"))
    compileOnly(files("C:/AIRSdks/AIRSDK_51.1.3.10/lib/android/lib/runtimeClasses.jar"))
}

// Merge `embedded` JARs into the AAR's classes.jar at bundle time. The
// AAR built by AGP places our compiled sources in classes.jar; we open
// it back up, add the embedded JAR contents (excluding classes our
// source already provides — notably the upstream
// org.xbill.DNS.AsyncSemaphore which we replace), and rewrite. AGP's
// own bundle task runs first, then our inject task patches the result
// in place before the AAR is published.
android {
    libraryVariants.configureEach {
        val variantName = name
        val capName = name.replaceFirstChar { it.titlecase() }
        val bundleTaskName = "bundle${capName}Aar"
        val injectTaskName = "embed${capName}IntoAar"

        // Classes our project source provides — the merge MUST exclude
        // these from the embedded JARs to avoid duplicate-class errors.
        // Path syntax matches the Java internal name + `.class` for use
        // with `Predicate<ZipEntry>`.
        val classesProvidedByProject = listOf(
            "org/xbill/DNS/AsyncSemaphore.class",
            "org/xbill/DNS/AsyncSemaphore\$Permit.class",
        )

        val embedTask = tasks.register(injectTaskName) {
            dependsOn(bundleTaskName, "${capName.replaceFirstChar { it.lowercase() }}AssetsCopyForAGP".takeIf { false } ?: bundleTaskName)
            inputs.files(embedded)
            outputs.upToDateWhen { false }
            doLast {
                val aar = tasks.named(bundleTaskName).get().outputs.files.singleFile
                val workDir = layout.buildDirectory.dir("intermediates/embed-${variantName}").get().asFile
                workDir.deleteRecursively()
                workDir.mkdirs()

                // Unpack AAR
                copy {
                    from(zipTree(aar))
                    into(workDir)
                }
                val classesJar = workDir.resolve("classes.jar")
                if (!classesJar.exists()) {
                    throw GradleException("AAR missing classes.jar: $aar")
                }

                // Stage classes.jar contents
                val classesDir = workDir.resolve("classes-merged")
                classesDir.mkdirs()
                copy {
                    from(zipTree(classesJar))
                    into(classesDir)
                }

                // Merge each embedded JAR, skipping classes the project provides.
                embedded.forEach { jar ->
                    logger.lifecycle("Embedding ${jar.name} into AAR (variant=$variantName)")
                    copy {
                        from(zipTree(jar))
                        into(classesDir)
                        eachFile {
                            val rel = relativePath.pathString.replace('\\', '/')
                            if (classesProvidedByProject.contains(rel)) {
                                logger.lifecycle("  skipping $rel (project source overrides)")
                                exclude()
                            }
                        }
                        // Drop META-INF/MANIFEST.MF and signing artifacts
                        exclude("META-INF/MANIFEST.MF")
                        exclude("META-INF/*.SF")
                        exclude("META-INF/*.DSA")
                        exclude("META-INF/*.RSA")
                        exclude("module-info.class")
                    }
                }

                // Re-jar classes
                val mergedJar = workDir.resolve("classes-merged.jar")
                ant.invokeMethod("jar", mapOf("destfile" to mergedJar.absolutePath, "basedir" to classesDir.absolutePath))
                classesJar.delete()
                mergedJar.renameTo(classesJar)

                // Re-zip AAR
                val patchedAar = workDir.resolve("patched.aar")
                ant.invokeMethod("zip", mapOf("destfile" to patchedAar.absolutePath, "basedir" to workDir.absolutePath, "excludes" to "classes-merged/**,classes-merged.jar,patched.aar"))
                aar.delete()
                patchedAar.renameTo(aar)
                logger.lifecycle("Patched AAR with embedded JARs: $aar")
            }
        }
        tasks.named("assemble${capName}").configure { dependsOn(embedTask) }
    }
}