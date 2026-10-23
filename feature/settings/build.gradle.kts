plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(libs.androidx.datastore.preferences)
}
