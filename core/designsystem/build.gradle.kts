plugins {
    id("parallax.android.library")
    id("parallax.android.compose")
    alias(libs.plugins.roborazzi)
}

dependencies {
    // The design system owns error and empty-state presentation, so it needs the error type
    // and the formatting helpers. `api` because ErrorState appears in consumers' signatures.
    api(projects.core.common)

    api(libs.compose.ui)
    api(libs.compose.material3)
    api(libs.compose.foundation)
    api(libs.compose.animation)
    implementation(libs.androidx.core.ktx)
    implementation(libs.compose.material.icons.core)
    implementation(libs.androidx.graphics.shapes)

    testImplementation(libs.robolectric)
    testImplementation(libs.roborazzi)
    testImplementation(libs.roborazzi.compose)
    testImplementation(libs.roborazzi.junit.rule)
    testImplementation(libs.compose.ui.test.junit4)
    testImplementation(libs.androidx.test.ext.junit)
}
