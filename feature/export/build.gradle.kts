plugins {
    id("parallax.android.feature")
    // The export options and the engine's answers cross the JNI boundary as
    // JSON, so this module needs the serialization compiler plugin: without it
    // the @Serializable classes have no generated serialisers and every export
    // fails at runtime with a SerializationException.
    alias(libs.plugins.kotlin.serialization)
}

dependencies {
    implementation(projects.core.data)
    implementation(projects.engine.native)
    implementation(libs.kotlinx.serialization.json)

    implementation(libs.androidx.documentfile)

    testImplementation(projects.core.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
    testImplementation(libs.androidx.room.runtime)
    testImplementation(libs.androidx.room.ktx)
    testImplementation(libs.androidx.datastore.preferences)
}
