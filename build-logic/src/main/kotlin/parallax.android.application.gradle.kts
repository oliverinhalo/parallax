import com.android.build.api.dsl.ApplicationExtension
import org.gradle.kotlin.dsl.configure

plugins {
    id("com.android.application")
}

extensions.configure<ApplicationExtension> {
    namespace = "uk.co.jacoblevy.parallax"
    compileSdk = parallaxLibs.int("compileSdk")

    defaultConfig {
        applicationId = "uk.co.jacoblevy.parallax"
        minSdk = parallaxLibs.int("minSdk")
        targetSdk = parallaxLibs.int("targetSdk")
        versionCode = 1
        versionName = "1.0.0"
        testInstrumentationRunner = "uk.co.jacoblevy.parallax.ParallaxTestRunner"
        // Parallax ships native code for 64-bit ABIs only: armeabi-v7a devices
        // lack the RAM and the Vulkan support the pipeline needs, and shipping
        // the ABI would halve the usable memory budget for no benefit.
        ndk {
            abiFilters += setOf("arm64-v8a", "x86_64")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            // Signing is wired from environment variables in app/build.gradle.kts
            // so no keystore or password ever lands in the repository.
        }
    }

    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            isReturnDefaultValues = true
        }
    }

    packaging {
        resources {
            excludes += setOf(
                "/META-INF/{AL2.0,LGPL2.1}",
                "/META-INF/LICENSE*",
                "/META-INF/DEPENDENCIES",
                "META-INF/versions/9/previous-compilation-data.bin",
            )
        }
        jniLibs {
            // The reconstruction engine is loaded with System.loadLibrary, so it
            // must stay uncompressed for the linker to mmap it directly.
            useLegacyPackaging = false
        }
    }
}

kotlin {
    compilerOptions {
        freeCompilerArgs.addAll("-jvm-default=enable")
    }
}

dependencies {
    "implementation"(parallaxLibs.findLibrary("kotlinx-coroutines-android").get())
    "testImplementation"(parallaxLibs.findLibrary("junit").get())
    "testImplementation"(parallaxLibs.findLibrary("truth").get())
    "testImplementation"(parallaxLibs.findLibrary("kotlinx-coroutines-test").get())
}
