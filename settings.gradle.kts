pluginManagement {
    includeBuild("build-logic")
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        // Google's read-only mirror of Maven Central. Listed first because the
        // canonical host aggressively rate-limits shared/CI egress IPs with
        // HTTP 429, which breaks dependency resolution mid-build; repo1 stays
        // as the fallback so nothing is unresolvable if the mirror lags.
        maven("https://maven-central.storage-download.googleapis.com/maven2/") {
            name = "GoogleMavenCentralMirror"
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode = RepositoriesMode.FAIL_ON_PROJECT_REPOS
    repositories {
        google()
        // Google's read-only mirror of Maven Central. Listed first because the
        // canonical host aggressively rate-limits shared/CI egress IPs with
        // HTTP 429, which breaks dependency resolution mid-build; repo1 stays
        // as the fallback so nothing is unresolvable if the mirror lags.
        maven("https://maven-central.storage-download.googleapis.com/maven2/") {
            name = "GoogleMavenCentralMirror"
        }
        mavenCentral()
    }
}

rootProject.name = "Parallax"

enableFeaturePreview("TYPESAFE_PROJECT_ACCESSORS")

include(":app")

// Shared foundations.
include(":core:common")
include(":core:model")
include(":core:designsystem")
include(":core:data")
include(":core:testing")
include(":core:ar")

// User-facing features. Each owns its navigation graph and ViewModels.
include(":feature:onboarding")
include(":feature:library")
include(":feature:capture")
include(":feature:processing")
include(":feature:viewer")
include(":feature:ar")
include(":feature:export")
include(":feature:settings")

// Reconstruction engine: native C++ core and the on-device ML models.
include(":engine:native")
include(":engine:ml")

// Macrobenchmarks (startup, viewer frame timing).
include(":benchmark")
