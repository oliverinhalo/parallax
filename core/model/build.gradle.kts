plugins {
    id("parallax.android.library")
    alias(libs.plugins.kotlin.serialization)
}

dependencies {
    api(libs.kotlinx.serialization.json)
    implementation(projects.core.common)
}
