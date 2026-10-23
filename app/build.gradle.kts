import java.util.Properties

plugins {
    id("parallax.android.application")
    id("parallax.android.compose")
    id("parallax.android.hilt")
}

android {
    buildFeatures {
        buildConfig = true
    }

    signingConfigs {
        create("release") {
            // Release signing is supplied entirely through the environment so
            // that no keystore, alias or password is ever committed. When the
            // variables are absent (local debug builds, forks, CI pull-request
            // runs) the config stays unpopulated and `assembleRelease` produces
            // an unsigned APK rather than failing the configuration phase.
            val storePath = System.getenv("PARALLAX_KEYSTORE")
            if (!storePath.isNullOrBlank() && file(storePath).exists()) {
                storeFile = file(storePath)
                storePassword = System.getenv("PARALLAX_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("PARALLAX_KEY_ALIAS")
                keyPassword = System.getenv("PARALLAX_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("release")
        }
    }
}

dependencies {
    implementation(projects.core.common)
    implementation(projects.core.model)
    implementation(projects.core.data)
    implementation(projects.core.designsystem)

    implementation(projects.feature.onboarding)
    implementation(projects.feature.library)
    implementation(projects.feature.capture)
    implementation(projects.feature.processing)
    implementation(projects.feature.viewer)
    implementation(projects.feature.ar)
    implementation(projects.feature.export)
    implementation(projects.feature.settings)

    implementation(projects.engine.native)
    implementation(projects.engine.ml)

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.hilt.navigation.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.work.runtime.ktx)

    androidTestImplementation(projects.core.testing)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.rules)
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.hilt.android.testing)
    kspAndroidTest(libs.hilt.compiler)
}
