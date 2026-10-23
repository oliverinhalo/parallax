plugins {
    id("parallax.android.library")
    id("parallax.android.hilt")
}

android {
    androidResources {
        // Model files must not be compressed or LiteRT cannot mmap them.
        noCompress += setOf("tflite", "litertmodel", "bin")
    }
}

dependencies {
    implementation(projects.core.common)
    implementation(projects.core.model)

    implementation(libs.litert)
    implementation(libs.litert.gpu)
    implementation(libs.litert.support)
    implementation(libs.androidx.core.ktx)

    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.ext.junit)
}
