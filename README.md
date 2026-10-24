# Parallax

**Capture the world in depth.**

An on-device 3D scanner for Android. Point the camera at an object, a room or a
space, walk around it, and get a measured, textured 3D model — a mesh, a point
cloud, a Gaussian splat field and a floor plan — without anything leaving the
phone.

Everything runs locally. There is no account, no sync, no backend, and the app
has no `INTERNET` permission at all, so a scan physically cannot be uploaded.
It works in airplane mode, including the first launch.

---

## What it does

**Four ways to capture.** Object mode for a thing on a table, Room mode for the
space you are standing in, Area mode for something larger, and Import for photos
or video you already have. The first three use ARCore where it is available; a
device without it can still import.

**Guidance while you scan.** A coverage ring fills in as you go and tells you
what you have missed — the underside, the top, the back. Frames are scored for
sharpness and parallax as they arrive, and only the ones that add something are
kept. The app watches the device's temperature, battery and free space, and says
so before any of them becomes a problem.

**Processing you can leave running.** Eleven stages, each checkpointed to disk,
in a foreground service with a real progress bar and a time estimate. Kill the
app, run out of battery, take a phone call — it resumes from the last completed
stage rather than starting again. Scans can be queued and processed later.

**A viewer that measures.** Orbit with inertia, or walk through a room scan at
eye height. Switch between shaded, textured, wireframe, point cloud and splat.
Tap to measure distances, heights and areas; drop annotated pins; crop, re-level
and rescale without ever rewriting the scan.

**Thirteen export formats.** GLB, glTF, OBJ, USDZ, STL, PLY, 3DGS PLY, `.splat`,
`.spz`, floor plans as SVG, PNG or PDF, and a self-contained HTML file that
opens in any browser with no network — geometry, texture and renderer all inside
the one file. Plus a `.parallax` bundle that moves a whole scan to another
device.

**Place it in the room.** Put a finished scan back into the space you are
standing in, at life size or as a tabletop model.

---

## Building it

You need JDK 17, the Android SDK with NDK 28.2.13676358 and CMake 3.22.1, and a
device running Android 10 (API 29) or newer.

```bash
git clone <this repository>
cd parallax
./gradlew :app:assembleDebug
```

The NDK's `glslc` compiles the Gaussian splat compute shaders during the build.
It ships with the NDK, so there is nothing extra to install.

### Running the checks

```bash
./gradlew build detekt          # assemble, unit tests, Lint, R8, detekt
./gradlew recordRoborazziDebug  # re-record the screenshot baselines
./gradlew verifyRoborazziDebug  # compare against them
```

### The native engine on its own

The reconstruction engine builds and tests on a host machine, which is where
most of its development happens — no device, no emulator, a few seconds a run.

```bash
cmake -S engine/native/cpp -B build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPARALLAX_BUILD_TESTS=ON -DPARALLAX_BUILD_TOOLS=ON
cmake --build build-host --parallel
./build-host/parallax_tests
./build-host/parallax_bench --runs 5
```

### Release signing

Signing credentials come from the environment and are never in the repository:

```bash
export PARALLAX_KEYSTORE=/path/to/keystore.jks
export PARALLAX_KEYSTORE_PASSWORD=…
export PARALLAX_KEY_ALIAS=…
export PARALLAX_KEY_PASSWORD=…
./gradlew :app:assembleRelease
```

Without them the release build still assembles — unsigned, which is what CI
wants and what a local build should get rather than an error.

---

## How it is put together

Multi-module, MVI, Hilt, Coroutines and Flow, with the reconstruction engine in
C++ behind a thin JNI bridge.

```
:app                 navigation, theme, process lifecycle
:feature:*           onboarding, library, capture, processing,
                     viewer, export, ar, settings
:core:model          domain types — plain Kotlin, no Android
:core:data           Room, the filesystem layout, project bundles
:core:designsystem   the design system
:core:ar             the camera-background shader capture and AR share
:core:common         errors, dispatchers, formatting
:engine:native       the JNI bridge
:engine:ml           LiteRT
engine/native/cpp    the engine: SfM, MVS, fusion, meshing, texturing,
                     splats, and every exporter
```

The design principle behind the layout is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the parts that cannot be tested in
CI — ARCore, Filament, Vulkan, the camera — are each confined to a single file,
and everything else is ordinary testable code.

---

## Where things stand

337 native assertions and 386 JVM tests run on every build, alongside detekt,
Android Lint, R8 minification and screenshot comparison. `PROGRESS.md` records
what each phase built and, more usefully, the bugs each phase's tests caught.

Some things genuinely cannot be verified in CI, and the app is built so that
being wrong about them degrades rather than breaks:

- **The Vulkan splat trainer** is compile-verified but has never been executed.
  A device that cannot run it falls back to the CPU implementation that the
  tests do cover, on a shorter schedule.
- **ARCore and Filament** are each behind one class. A renderer that refuses to
  start leaves the viewer saying so, with measurements and export still working.
- **Monocular depth** needs a model the user supplies; without one, depth comes
  from the sensor or from multi-view stereo.

The honest list is in `PROGRESS.md` under "verification boundaries".

---

## Privacy

Nothing is collected and nothing is sent. No analytics, no crash reporting, no
telemetry of any kind. The app does not request the `INTERNET` permission, which
means this is enforced by the platform rather than by intent. Scans live in the
app's private storage and leave only when you share or export them.

---

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how it fits together, with
  diagrams.
- [docs/DECISIONS.md](docs/DECISIONS.md) — fourteen decisions and what each one
  cost.
- [docs/LICENSES.md](docs/LICENSES.md) — every dependency and its licence.
- [PROGRESS.md](PROGRESS.md) — the build log, phase by phase.
- [CLAUDE.md](CLAUDE.md) — conventions, for anyone (or anything) working in this
  repository.
