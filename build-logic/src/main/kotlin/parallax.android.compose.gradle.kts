import com.android.build.api.dsl.CommonExtension
import org.gradle.kotlin.dsl.configure

plugins {
    id("org.jetbrains.kotlin.plugin.compose")
}

extensions.configure<CommonExtension> {
    buildFeatures.compose = true
}

dependencies {
    val bom = platform(parallaxLibs.findLibrary("compose-bom").get())
    "implementation"(bom)
    "androidTestImplementation"(bom)

    "implementation"(parallaxLibs.findLibrary("compose-ui").get())
    "implementation"(parallaxLibs.findLibrary("compose-ui-graphics").get())
    "implementation"(parallaxLibs.findLibrary("compose-ui-tooling-preview").get())
    "implementation"(parallaxLibs.findLibrary("compose-foundation").get())
    "implementation"(parallaxLibs.findLibrary("compose-material3").get())
    "implementation"(parallaxLibs.findLibrary("compose-animation").get())
    "implementation"(parallaxLibs.findLibrary("compose-runtime").get())
    "implementation"(parallaxLibs.findLibrary("androidx-lifecycle-runtime-compose").get())
    "implementation"(parallaxLibs.findLibrary("androidx-lifecycle-viewmodel-compose").get())

    "debugImplementation"(parallaxLibs.findLibrary("compose-ui-tooling").get())
    "debugImplementation"(parallaxLibs.findLibrary("compose-ui-test-manifest").get())
    "androidTestImplementation"(parallaxLibs.findLibrary("compose-ui-test-junit4").get())
}
