// Convention plugins live in a separate *included* build rather than in
// buildSrc. The difference matters: buildSrc's `implementation` dependencies
// land on the root project's buildscript classpath, which makes every versioned
// `alias(libs.plugins.*)` in the main build fail with "already on the classpath
// with an unknown version". An included build keeps its classpath to itself.
pluginManagement {
    repositories {
        google()
        maven("https://maven-central.storage-download.googleapis.com/maven2/") {
            name = "GoogleMavenCentralMirror"
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        maven("https://maven-central.storage-download.googleapis.com/maven2/") {
            name = "GoogleMavenCentralMirror"
        }
        mavenCentral()
        gradlePluginPortal()
    }
    versionCatalogs {
        create("libs") {
            from(files("../gradle/libs.versions.toml"))
        }
    }
}

rootProject.name = "build-logic"
