plugins {
    id("com.google.devtools.ksp")
}

dependencies {
    "implementation"(parallaxLibs.findLibrary("androidx-room-runtime").get())
    "implementation"(parallaxLibs.findLibrary("androidx-room-ktx").get())
    "ksp"(parallaxLibs.findLibrary("androidx-room-compiler").get())
    "testImplementation"(parallaxLibs.findLibrary("androidx-room-testing").get())
}

// Export the generated schema so migrations can be verified in CI and the
// MigrationTestHelper can replay real upgrade paths.
ksp {
    arg("room.schemaLocation", "${projectDir}/schemas")
    arg("room.generateKotlin", "true")
}
