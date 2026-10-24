plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)

    testImplementation(projects.core.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
    // The ViewModel tests drive the real repositories against an in-memory database rather
    // than fakes: the behaviour that matters here is the interaction between a filter, a
    // selection and a delete, and a fake repository would only assert that the ViewModel
    // calls the methods this ViewModel calls.
    testImplementation(libs.androidx.room.runtime)
    testImplementation(libs.androidx.room.ktx)
}
