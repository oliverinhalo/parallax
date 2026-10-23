plugins {
    id("parallax.android.library")
}

dependencies {
    api(libs.junit)
    api(libs.truth)
    api(libs.kotlinx.coroutines.test)
    api(libs.turbine)
    api(libs.mockk)
    api(projects.core.model)
    implementation(projects.core.common)
    implementation(libs.androidx.test.ext.junit)
}
