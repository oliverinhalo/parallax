plugins { id("parallax.android.library") }

dependencies {
    // ARCore only. This module exists because capture and AR placement both
    // have to draw the camera feed under their own content, and that is one
    // shader either way; everything else about the two sessions differs enough
    // that sharing it would be a pile of flags.
    implementation(libs.arcore)
}
