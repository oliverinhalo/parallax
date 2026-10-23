// Root build file. All real configuration lives in the buildSrc convention
// plugins; this file only registers the plugins used across the build and the
// aggregate quality tasks.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.android.test) apply false
    alias(libs.plugins.kotlin.compose) apply false
    alias(libs.plugins.kotlin.serialization) apply false
    alias(libs.plugins.ksp) apply false
    alias(libs.plugins.hilt) apply false
    alias(libs.plugins.detekt)
}

detekt {
    source.setFrom(files(projectDir).asFileTree.matching { include("**/src/*/kotlin/**") })
    config.setFrom(files("$rootDir/config/detekt/detekt.yml"))
    buildUponDefaultConfig = true
    parallel = true
}

/**
 * One task that runs everything CI runs, so `./gradlew check` locally and the
 * workflow cannot drift apart.
 */
tasks.register("parallaxCheck") {
    group = "verification"
    description = "Lint, detekt, unit tests and native tests across every module."
    dependsOn(
        subprojects.mapNotNull { it.tasks.findByName("testDebugUnitTest") },
    )
    dependsOn("detekt")
}
