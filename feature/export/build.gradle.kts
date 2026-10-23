plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)

    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.work.runtime.ktx)
}
