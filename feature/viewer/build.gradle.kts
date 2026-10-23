plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)

    api(libs.filament.android)
    implementation(libs.filament.utils.android)
    implementation(libs.filament.gltfio.android)
}
