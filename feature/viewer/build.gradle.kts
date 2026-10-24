plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)

    testImplementation(projects.core.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
    // The ViewModel tests drive the real repositories against an in-memory
    // database: the behaviour that matters is how a measurement, an edit and a
    // render-mode change interact, and fakes would only assert the calls made.
    testImplementation(libs.androidx.room.runtime)
    testImplementation(libs.androidx.room.ktx)
    testImplementation(libs.androidx.datastore.preferences)

    api(libs.filament.android)
    implementation(libs.filament.utils.android)
    implementation(libs.filament.gltfio.android)
}
