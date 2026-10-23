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

    // Release signing is supplied entirely through the environment so that no
    // keystore, alias or password is ever committed. When the variables are
    // absent -- local builds, forks, CI pull-request runs -- no signing config
    // is created at all and `assembleRelease` produces an unsigned APK.
    // Creating an empty config and assigning it would instead fail the build in
    // `validateSigningRelease`, which is a confusing way to say "no keystore".
    val keystorePath: String? = System.getenv("PARALLAX_KEYSTORE")
    val keystore = keystorePath?.takeIf { it.isNotBlank() }?.let(::file)?.takeIf { it.exists() }

    if (keystore != null) {
        signingConfigs {
            create("release") {
                storeFile = keystore
                storePassword = System.getenv("PARALLAX_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("PARALLAX_KEY_ALIAS")
                keyPassword = System.getenv("PARALLAX_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release")
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

    implementation(libs.arcore)

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
