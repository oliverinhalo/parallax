plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.core.ar)
    implementation(projects.feature.viewer)
    implementation(projects.engine.native)

    testImplementation(projects.core.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)

    implementation(libs.arcore)
    implementation(libs.filament.android)
    implementation(libs.filament.utils.android)
}
