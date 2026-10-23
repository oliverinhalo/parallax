// Convention plugins. Keeping the shared Android/Compose/Hilt configuration here
// means the 16 feature and core modules declare intent ("I am a feature module")
// rather than repeating 40 lines of boilerplate each.
plugins {
    `kotlin-dsl`
}

group = "uk.co.jacoblevy.parallax.buildlogic"

dependencies {
    // These are `implementation`, not `compileOnly`. Precompiled script plugins
    // are compiled into this jar and loaded by Gradle from it, so the DSL types
    // they reference must be on the jar's runtime classpath -- `compileOnly`
    // compiles fine and then fails at plugin application with
    // NoClassDefFoundError. Because this is an included build, these do not
    // leak onto the consuming build's classpath.
    implementation(libs.android.gradlePlugin)
    implementation(libs.ksp.gradlePlugin)
    implementation(libs.compose.gradlePlugin)
    implementation(libs.hilt.gradlePlugin)
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}
