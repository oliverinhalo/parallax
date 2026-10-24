# Licences

Parallax is built to be shippable commercially, so every dependency carries a
permissive licence — MIT, BSD, Apache-2.0 or MPL-2.0. **Nothing here is GPL or
AGPL**, which is why some obvious-looking choices were passed over: OpenMVS
(AGPL-3.0) for multi-view stereo, and the larger Depth Anything V2 weights
(CC-BY-NC-4.0) for monocular depth. Both were replaced with implementations or
models that can be shipped; see [DECISIONS.md](DECISIONS.md) for what that cost.

Versions are the ones pinned in `gradle/libs.versions.toml` and
`engine/native/cpp/third_party/`. When a dependency is added, it goes in this
table in the same change.

## How to check this yourself

```bash
# Every Maven coordinate the app actually resolves, by module.
./gradlew :app:dependencies --configuration releaseRuntimeClasspath

# The vendored native sources and their licence files.
ls engine/native/cpp/third_party/*/{LICENSE,COPYING,LICENSE.txt} 2>/dev/null
```

---

## Build tooling

These are needed to build Parallax and are not shipped inside it.

| Dependency | Version | Licence |
|---|---|---|
| Android Gradle Plugin | 9.4.1 | Apache-2.0 |
| Gradle | 9.8.2 | Apache-2.0 |
| Kotlin (compiler, stdlib, serialization plugin) | 2.4.21 | Apache-2.0 |
| KSP | 2.3.12 | Apache-2.0 |
| Android NDK (clang, libc++, `glslc`) | 28.2.13676358 | NDK licence; components are Apache-2.0, NCSA and BSD |
| CMake | 3.22.1 | BSD-3-Clause |
| detekt | 1.23.8 | Apache-2.0 |

`glslc` compiles the Gaussian splat compute shaders to SPIR-V at build time.
It ships with the NDK, so there is nothing extra to install.

## Shipped in the APK

### AndroidX and Jetpack Compose

Everything under `androidx.*` is **Apache-2.0**, including the Compose BOM
(2026.09.00) and everything it resolves.

| Dependency | Version | Licence |
|---|---|---|
| androidx.core:core-ktx | 1.19.1 | Apache-2.0 |
| androidx.core:core-splashscreen | 1.2.0 | Apache-2.0 |
| androidx.activity:activity-compose | 1.13.0 | Apache-2.0 |
| androidx.lifecycle:* | 2.11.0 | Apache-2.0 |
| androidx.navigation:navigation-compose | 2.10.2 | Apache-2.0 |
| androidx.room:* | 2.8.5 | Apache-2.0 |
| androidx.datastore:datastore-preferences | 1.3.0 | Apache-2.0 |
| androidx.work:work-runtime-ktx | 2.12.0 | Apache-2.0 |
| androidx.camera:* (CameraX) | 1.6.2 | Apache-2.0 |
| androidx.exifinterface:exifinterface | 1.4.2 | Apache-2.0 |
| androidx.documentfile:documentfile | 1.1.0 | Apache-2.0 |
| androidx.graphics:graphics-shapes | 1.0.1 | Apache-2.0 |
| androidx.hilt:hilt-* | 1.4.0 | Apache-2.0 |
| Compose BOM (ui, material3, foundation, animation, icons) | 2026.09.00 | Apache-2.0 |

### Dependency injection and concurrency

| Dependency | Version | Licence |
|---|---|---|
| com.google.dagger:hilt-android | 2.60.1 | Apache-2.0 |
| org.jetbrains.kotlinx:kotlinx-coroutines-android | 1.11.0 | Apache-2.0 |
| org.jetbrains.kotlinx:kotlinx-serialization-json | 1.12.0 | Apache-2.0 |

### 3D, AR and machine learning

| Dependency | Version | Licence | Notes |
|---|---|---|---|
| com.google.ar:core (ARCore) | 1.57.0 | Apache-2.0 | The SDK is Apache-2.0; the ARCore *service* it talks to is a Google Play component with its own terms, so an app using it is subject to those as well as this licence. Optional at runtime — a device without it keeps photo and video import, processing, the viewer and every export |
| com.google.android.filament:filament-android | 1.77.3 | Apache-2.0 | The viewer's renderer |
| com.google.android.filament:filament-utils-android | 1.77.3 | Apache-2.0 | |
| com.google.android.filament:gltfio-android | 1.77.3 | Apache-2.0 | glTF loading |
| com.google.ai.edge.litert:litert | 2.3.0 | Apache-2.0 | On-device inference |

### Model weights

| Model | Licence | Notes |
|---|---|---|
| Depth Anything V2 **Small** | Apache-2.0 | The only variant used |
| Depth Anything V2 Base / Large | CC-BY-NC-4.0 | **Not used.** Non-commercial, so they cannot ship here however much better they are |

No model weights are bundled in the APK and none are downloaded. Monocular
depth runs only if the user supplies a converted Small model through the
settings screen; without one, depth comes from the sensor or from the engine's
own multi-view stereo. See [DECISIONS.md](DECISIONS.md), ADR-008.

### Native engine

Vendored under `engine/native/cpp/third_party/`, so a build needs no network.

| Dependency | Version | Licence | Notes |
|---|---|---|---|
| Eigen | 3.4 | MPL-2.0 | Built with `EIGEN_MPL2_ONLY`, which excludes the LGPL-licensed corners of the library |
| zlib | NDK sysroot | zlib licence | PNG compression and the ZIP containers behind `.usdz`, `.spz` and `.parallax` |
| GoogleTest | 1.14 | BSD-3-Clause | Tests only; not in the APK |

Everything else in the engine is written here: structure from motion, bundle
adjustment, multi-view stereo, TSDF fusion, marching tetrahedra, texturing,
floor-plan extraction, Gaussian splat training, the baseline JPEG decoder, the
PNG encoder, the JSON parser, the ZIP writer, and every exporter — glTF, GLB,
OBJ, USDZ, PLY, STL, 3DGS PLY, `.splat`, `.spz`, SVG, PDF and the HTML viewer.

That is a lot to write rather than pull in, and it was a deliberate trade. The
formats are specified and stable, a writer is far less code than a reader, and
it keeps the licence surface to three entries. ADR-010 records the reasoning.

### Fonts

| Font | Licence | File |
|---|---|---|
| Inter | SIL Open Font License 1.1 | [licenses/Inter-OFL.txt](licenses/Inter-OFL.txt) |
| JetBrains Mono | SIL Open Font License 1.1 | [licenses/JetBrainsMono-OFL.txt](licenses/JetBrainsMono-OFL.txt) |

Both are bundled rather than downloaded, because the app must look right in
airplane mode on first launch. The OFL permits bundling and requires the
licence text to travel with the fonts, which is what the files above are for;
they are also shown in the app under Settings → Licences.

## Testing and quality

Not shipped in the APK.

| Dependency | Version | Licence |
|---|---|---|
| junit:junit | 4.13.2 | EPL-1.0 |
| androidx.test:* | 1.7.0 / 1.3.0 | Apache-2.0 |
| androidx.test.espresso:espresso-core | 3.7.0 | Apache-2.0 |
| androidx.test.uiautomator:uiautomator | 2.4.0 | Apache-2.0 |
| androidx.benchmark:benchmark-macro-junit4 | 1.5.0 | Apache-2.0 |
| com.google.truth:truth | 1.4.5 | Apache-2.0 |
| app.cash.turbine:turbine | 1.2.1 | Apache-2.0 |
| io.mockk:mockk | 1.14.11 | Apache-2.0 |
| org.robolectric:robolectric | 4.17 | MIT |
| io.github.takahirom.roborazzi:* | 1.77.0 | Apache-2.0 |

JUnit 4 is EPL-1.0, a weak copyleft licence that applies per-file. It is a test
dependency and no JUnit code is modified or distributed, so it has no effect on
the app's licensing.

## Deliberate omissions

| Not used | Licence | Why, and what replaced it |
|---|---|---|
| OpenMVS | AGPL-3.0 | Multi-view stereo is implemented in `engine/native/cpp/src/depth/MultiViewStereo.cpp` instead: a plane sweep with NCC matching, geometric consistency and inverse-depth sampling |
| COLMAP | BSD, but GPL via its Ceres/CGAL paths | Structure from motion and bundle adjustment are implemented here |
| libjpeg-turbo | BSD-3-Clause (usable) | A baseline decoder is implemented here instead, validated against libjpeg-turbo's own output on eleven fixtures. One fewer native dependency to cross-compile, and the decoder is 600 lines |
| Depth Anything V2 Base/Large | CC-BY-NC-4.0 | The Small variant, which is Apache-2.0 |
| Google Play Services (analytics, crash reporting) | Proprietary | Nothing is collected, so there is nothing to report. The app has no `INTERNET` permission |

## Parallax itself

Copyright © Jacob Levy. No licence has been chosen for this source yet, so no
`LICENSE` file is in the repository — which means the default applies and all
rights are reserved. That is a decision for the owner to make, not something to
guess at; what this document establishes is that nothing Parallax depends on
would constrain it. Every dependency above permits commercial distribution of a
closed-source binary, and nothing requires the app's own source to be
published.
