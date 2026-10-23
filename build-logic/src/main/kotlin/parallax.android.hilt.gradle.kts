plugins {
    id("com.google.devtools.ksp")
    id("com.google.dagger.hilt.android")
}

dependencies {
    "implementation"(parallaxLibs.findLibrary("hilt-android").get())
    "ksp"(parallaxLibs.findLibrary("hilt-compiler").get())
}
