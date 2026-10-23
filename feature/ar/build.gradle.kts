plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.feature.viewer)
    implementation(projects.engine.native)

    implementation(libs.arcore)
    implementation(libs.filament.android)
    implementation(libs.filament.utils.android)
}
