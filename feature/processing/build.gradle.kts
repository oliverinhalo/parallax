plugins {
    id("parallax.android.feature")
    // The pipeline configuration and the stage outcomes cross the JNI boundary as JSON, so
    // this module needs the serialization plugin. Without it the generated serialisers do not
    // exist and the reflective lookup throws on the first reconstruction -- which compiles
    // perfectly and fails at runtime.
    alias(libs.plugins.kotlin.serialization)
}

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)
    implementation(projects.engine.ml)

    implementation(libs.kotlinx.serialization.json)

    implementation(libs.androidx.work.runtime.ktx)
    implementation(libs.androidx.hilt.work)
    implementation(libs.androidx.lifecycle.service)
    ksp(libs.androidx.hilt.compiler)

    testImplementation(projects.core.testing)
    testImplementation(libs.androidx.work.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
    testImplementation(libs.androidx.room.runtime)
    testImplementation(libs.androidx.room.ktx)
    testImplementation(libs.androidx.datastore.preferences)
}
