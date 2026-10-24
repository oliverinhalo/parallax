plugins { id("parallax.android.library") }

dependencies {
    // ARCore only. This module exists because capture and AR placement both
    // have to draw the camera feed under their own content, and that is one
    // shader either way; everything else about the two sessions differs enough
    // that sharing it would be a pile of flags. It also holds the one-session
    // rule, which has to sit above both claimants to mean anything.
    implementation(libs.arcore)
}
