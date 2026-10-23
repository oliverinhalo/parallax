import com.android.build.api.dsl.LibraryExtension
import org.gradle.kotlin.dsl.configure

plugins {
    id("com.android.library")
}

extensions.configure<LibraryExtension> {
    namespace = defaultNamespace()
    compileSdk = parallaxLibs.int("compileSdk")

    defaultConfig {
        minSdk = parallaxLibs.int("minSdk")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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
    }
}

kotlin {
    compilerOptions {
        // Interface default methods are compiled into the interface rather than
        // into synthetic DefaultImpls classes, which keeps the repository
        // interfaces in :core:data free of abstract-member churn. The flag was
        // renamed from -Xjvm-default in Kotlin 2.2.
        freeCompilerArgs.addAll("-jvm-default=enable")
        allWarningsAsErrors = providers.gradleProperty("parallax.warningsAsErrors")
            .map(String::toBoolean).orElse(false)
    }
}

dependencies {
    "implementation"(parallaxLibs.findLibrary("kotlinx-coroutines-core").get())
    "testImplementation"(parallaxLibs.findLibrary("junit").get())
    "testImplementation"(parallaxLibs.findLibrary("truth").get())
    "testImplementation"(parallaxLibs.findLibrary("kotlinx-coroutines-test").get())
}
