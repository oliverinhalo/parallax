plugins {
    id("parallax.android.library")
}

android {
    // `:engine:native` would derive `...engine.native`, and `native` is a Java
    // keyword. Pin it to the package the Kotlin bridge actually uses.
    namespace = "uk.co.jacoblevy.parallax.engine.nativebridge"

    defaultConfig {
        // Match the app's ABI set so CI does not spend time cross-compiling
        // Eigen for ABIs that are never packaged.
        ndk {
            abiFilters += setOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // -frtti/-fexceptions: Eigen uses both. NEON is baseline on
                // arm64-v8a so no runtime dispatch is needed.
                cppFlags += listOf("-std=c++17", "-frtti", "-fexceptions", "-O3", "-DNDEBUG")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("cpp/CMakeLists.txt")
            version = libs.versions.cmake.get()
        }
    }
    ndkVersion = libs.versions.ndk.get()
}

dependencies {
    implementation(projects.core.common)
    implementation(projects.core.model)
}
