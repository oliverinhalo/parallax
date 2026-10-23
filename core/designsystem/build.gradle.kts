plugins {
    id("parallax.android.library")
    id("parallax.android.compose")
    alias(libs.plugins.roborazzi)
}

dependencies {
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
