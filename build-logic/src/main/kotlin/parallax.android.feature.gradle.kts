plugins {
    id("parallax.android.library")
    id("parallax.android.compose")
    id("parallax.android.hilt")
}

dependencies {
    "implementation"(project(":core:designsystem"))
    "implementation"(project(":core:model"))
    "implementation"(project(":core:common"))

    "implementation"(parallaxLibs.findLibrary("androidx-navigation-compose").get())
    "implementation"(parallaxLibs.findLibrary("androidx-hilt-navigation-compose").get())
    "implementation"(parallaxLibs.findLibrary("androidx-lifecycle-runtime-ktx").get())

    "testImplementation"(parallaxLibs.findLibrary("turbine").get())
    "testImplementation"(parallaxLibs.findLibrary("mockk").get())
}
