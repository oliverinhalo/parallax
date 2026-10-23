plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(libs.androidx.activity.compose)

    testImplementation(projects.core.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
    testImplementation(libs.androidx.datastore.preferences)
}
