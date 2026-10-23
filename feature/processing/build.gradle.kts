plugins { id("parallax.android.feature") }

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)
    implementation(projects.engine.ml)

    implementation(libs.androidx.work.runtime.ktx)
    implementation(libs.androidx.hilt.work)
    implementation(libs.androidx.lifecycle.service)
    ksp(libs.androidx.hilt.compiler)

    testImplementation(libs.androidx.work.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
}
