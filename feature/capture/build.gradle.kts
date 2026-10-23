plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.ml)
    implementation(projects.engine.native)

    implementation(libs.androidx.camera.core)
    implementation(libs.androidx.camera.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.camera.video)
    implementation(libs.androidx.camera.view)
    implementation(libs.arcore)
    implementation(libs.androidx.exifinterface)
}
