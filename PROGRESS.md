# Parallax — build progress

On-device 3D scanner for Android. Point your phone at an object, a room or an
open space; get a measurable, exportable 3D model. No account, no cloud, no
upload.

**How to read this file.** Each phase lists what is done and *how it was
verified*. "Verified" means a test or a build that actually ran in this
repository, not an assertion. Anything that cannot be verified without a
physical device is marked explicitly — see
[Verification boundaries](#verification-boundaries).

---

## Toolchain (verified)

| Component | Version | Note |
|---|---|---|
| Android Gradle Plugin | 9.4.1 | requires Gradle >= 9.6.0, JDK >= 17 |
| Gradle | 9.8.2 | current stable |
| Kotlin | 2.4.21 | via AGP **built-in Kotlin**; `kotlin-android` is gone in AGP 9 |
| compileSdk / targetSdk | 37 | current AndroidX requires >= 37 |
| minSdk | 29 | |
| NDK | 28.2.13676358 | AGP 9.4's default, pinned |
| Compose BOM | 2026.09.00 | |

Every version was resolved against live Maven metadata rather than recalled.
Six of my initial guesses were wrong (detekt, Roborazzi, MockK, Turbine, Truth,
kotlinx-serialization) and are corrected in `gradle/libs.versions.toml`.

---

## Phase 1 — Foundation ✅

- [x] 16-module Gradle build: `:app`, 5 `:core:*`, 8 `:feature:*`, 2 `:engine:*`, `:benchmark`
- [x] Convention plugins in an included `build-logic` build
- [x] Version catalog, detekt config, `.gitignore`/`.gitattributes`
- [x] NDK/CMake wiring; `libparallax_engine.so` built for arm64-v8a and x86_64

**Verified:** `./gradlew :app:assembleDebug` succeeds (309 tasks); the APK
contains `lib/arm64-v8a/libparallax_engine.so` and `lib/x86_64/…`.

Three non-obvious things this phase settled, all of which cost real debugging:

1. **`build-logic`, not `buildSrc`.** buildSrc's `implementation` dependencies
   land on the root project's buildscript classpath, so every versioned
   `alias(libs.plugins.*)` then fails with *"already on the classpath with an
   unknown version"*. An included build keeps its classpath to itself.
2. **The convention plugins' catalog helper must not be called `libs`.** A
   `Project.libs` extension on the buildscript classpath shadows Gradle's
   generated type-safe accessor, breaking `libs.androidx.core.ktx` in every
   module. It is `parallaxLibs` here.
3. **Google's Maven Central mirror is listed first** in all four repository
   blocks. The canonical host returns HTTP 429 to shared CI egress IPs and
   breaks resolution mid-build.

## Phase 2 — Native engine core ✅

C++17, Eigen-only, built and tested on the host as well as cross-compiled.

- [x] **SE(3)/SO(3)** exp/log, composition, interpolation, gauge repair
- [x] **Features**: FAST-9 + Harris + NMS, intensity-centroid orientation,
      steered BRIEF-256, Hamming matching with ratio test and cross-check
- [x] **Frame scoring**: Laplacian variance, exposure, clipping, contrast;
      dHash near-duplicate rejection
- [x] **Two-view geometry**: normalised 8-point F, E, pose recovery by
      cheirality, homography, Sampson/symmetric-epipolar errors, planarity
      degeneracy detection
- [x] **RANSAC** driver with adaptive stopping and non-minimal refit
- [x] **Triangulation**: two-view and multi-view DLT with parallax gating
- [x] **PnP**: conditioned DLT, RANSAC, Gauss-Newton refinement with Huber
- [x] **Bundle adjustment**: sparse Levenberg-Marquardt with explicit Schur
      complement, Huber loss, pose/point gauge fixing, optional scale gauge
- [x] **TSDF fusion**: hashed 8³ blocks, incidence/range weighting, memory
      budget, trilinear sampling, checkpoint serialisation
- [x] **Isosurface extraction**: marching tetrahedra (Freudenthal 6-tet)
- [x] **Mesh ops**: weld, degenerate removal, connected components, boundary
      loops, hole filling, quadric decimation, Laplacian smoothing, LODs,
      transform, crop, watertight/Euler checks
- [x] **Synthetic datasets**: analytic ray casting over textured primitives,
      giving frames with pixel-exact ground-truth depth and known poses

**Verified: 134 gtest assertions pass.** Notable measured results:

- SE(3) exp/log round-trips to 1e-9 across the full rotation range, including
  the near-π branch.
- Essential-matrix estimation recovers ground-truth rotation to <0.1° on clean
  data and <2° with 30% outliers, rejecting >85% of those outliers.
- Bundle adjustment pulls an 8-camera/120-point problem perturbed by ~3 cm and
  ~0.7° back to **<0.05 px RMSE**, with analytic Jacobians agreeing with
  central differences to 1e-3 relative.
- TSDF fusion of a 72-view orbit reconstructs a 25 cm sphere to within
  **3 voxels mean radial error**; the extracted mesh is watertight with Euler
  characteristic 2.
- Quadric decimation to 25% of the triangle count holds surface area and
  volume within 6% and keeps the surface closed.

### Bugs this phase's tests caught

These are recorded because each was silent and would have been very hard to
find later:

| Bug | Symptom | Cause |
|---|---|---|
| `ComputeThinV` in the null-space solve | RANSAC never fit *any* model, two-view estimation always failed | Thin V has `min(rows, cols)` columns, so for a minimal 8×9 sample the solution column does not exist |
| FAST compass pre-test required 3 of 4 | Corners of axis-aligned rectangles were never detected | The "3 of 4" rule belongs to FAST-12; a 9-long arc only guarantees ⌊9/4⌋ = 2 |
| Near-π axis from `(R+I)/2` | SO(3) log inaccurate by O(π−θ) near π | Leaves an O(sin θ) skew term; symmetrising first removes it |
| Marching-tetrahedra winding from corner index order | ~40% of triangles inverted; signed volume meaningless | The six Freudenthal tetrahedra do not share a handedness |
| Feature budget "top-up" | All features in one image region, ill-conditioned BA | Global top-up after grid binning defeated the binning |
| Point-cloud normal fallback | Rim of wrong-facing normals at scan edges | Defaulted to (0,1,0) where central differences were unavailable |

---

## Phase 3 — Exporters, room features and texturing ✅

Folded into the native engine (commits `57f6015`, `d83ca91`, `7b71a2b`) because
every one of them is a pure function of a mesh or a point cloud, and keeping
them host-testable is worth more than module separation.

- **Every export format**, written by hand with no third-party writer: GLB,
  glTF + external buffer, OBJ + MTL + PNG, USDZ (64-byte-aligned store-only
  ZIP), STL, PLY, 3DGS PLY, `.splat`, `.spz` v2, floor plan SVG/PDF/PNG. Each
  is validated by reading the file back and comparing geometry.
- **Supporting codecs**, also by hand: JSON parser and writer, PNG encoder
  (zlib with adaptive row filters), ZIP writer, single-page PDF 1.4 writer.
- **Plane detection and floor plans**: RANSAC planes with a *signed* normal
  test, occupancy grid derived from the floor plane, morphological closing,
  hole filling, Douglas–Peucker, line-intersection regularisation, and 3D
  vertical-profile opening detection.
- **Texturing**: per-triangle atlas, view scoring, depth-map visibility, shelf
  packing with size-scaled gutters, Gauss–Seidel seam levelling, inpainting.

Measured: a 4.2 × 3.1 m synthetic room is measured at **13.00 m²** against a
true 13.02 m² (0.15% error). `.spz` round-trips through our own reader; it has
**not** been checked against the reference decoder (ADR-010).

---

## Phase 4 — Data layer ✅

`:core:data`, 58 unit tests, all passing (`./gradlew :core:data:test`).

- **Room schema v1**: projects, folders, annotations, measurements, processing
  jobs. Metadata only; binaries live on disk (ADR-005). Foreign keys cascade
  from a project to its annotations, measurements and job, and set null from a
  deleted folder to its projects, so deleting a folder never deletes a scan.
  The schema JSON is exported so the first migration has a real baseline.
- **`ProjectStorage`**: the single definition of the on-disk layout that is a
  contract with `Pipeline.cpp` — `manifest.json`, `frames/`, `depth/`,
  `checkpoints/`, `outputs/`, `exports/`. Manifest writes are atomic
  (temp + rename). Storage accounting, per-category breakdown for the settings
  dashboard, intermediate purge, and orphan reclamation.
- **`ProjectBundle`**: `.parallax` export and import as a plain ZIP, so a scan
  is recoverable with any unzip tool. Entry names are resolved against the
  destination and refused if they escape it; a failed import unwinds completely.
- **Repositories**: projects (the only writer of both stores), folders, the
  processing queue, and settings on DataStore. The queue is persisted so it
  survives process death, and interrupted jobs are released at startup while
  keeping the stage they had reached.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| `encodeDefaults` wrote `"pose": null` | An Import capture would have been read as an AR capture with every camera at the origin, with no error anywhere | The pipeline detects pose-less captures with `entry.has("pose")`, and a JSON null answers yes |
| Bundle import reused annotation ids | Importing a bundle exported from the same device *moved* the original's annotations onto the copy | Annotation and measurement ids are table-wide primary keys, not per project |
| Robolectric could not start | Every Robolectric test failed in application setup | JDK 17+ needs `--add-exports java.base/jdk.internal.access` for Robolectric's file-descriptor interception |
| `signingConfig` assigned when unset | `assembleRelease` failed with "supplied proguard configuration does not exist" and then a signing error, on a build that is meant to produce an unsigned APK | An empty signing config still runs `validateSigningRelease`; it is now not created at all when the environment has no keystore |

---

## Remaining phases

- [ ] **Phase 5 — Capture**: ARCore + CameraX, four modes, live guidance,
      coverage visualisation, frame scoring, device capability detection,
      thermal and battery awareness
- [ ] **Phase 6 — Depth & ML** (`:engine:ml`): sensor depth, monocular depth
      (Depth Anything V2 **Small**, Apache-2.0 only), scale alignment,
      subject segmentation
- [ ] **Phase 7 — Gaussian splatting**: Vulkan compute training and renderer
- [ ] **Phase 8 — Processing system**: WorkManager queue, foreground service,
      checkpoint/resume wiring, progress and ETA
- [ ] **Phase 9 — Viewer**: Filament, render modes, measurement, editing,
      annotations, walk mode
- [ ] **Phase 10 — Export UI, AR placement, offline HTML viewer**
- [ ] **Phase 11 — App shell**: navigation, onboarding, library, settings and
      storage dashboard
- [ ] **Phase 12 — Polish & hardening**: accessibility pass, screenshot tests,
      macrobenchmarks, CI, remaining docs

---

## Verification boundaries

Stated plainly so nothing here is mistaken for more than it is.

**Verified by tests that run in this repo:** everything in Phases 2, 3 and 4 —
255 native gtest assertions and 58 JVM/Robolectric tests — plus the Gradle
build, R8 release minification and APK packaging from Phase 1.

**Cannot be verified in this environment** (no physical device, no camera, no
GPU, no ARCore service):

- ARCore pose/depth/plane behaviour and anything built on it
- Camera capture and real camera intrinsics
- Vulkan compute kernels and the Gaussian-splat renderer
- Filament rendering output
- LiteRT model inference with GPU/NNAPI delegates
- Real-device frame rates, thermal throttling and memory behaviour

For these, the engine's host-testable parts are tested against synthetic data
and the device-only layers are kept as thin as possible, so that the logic that
*can* be tested is tested. Where an algorithm is device-only, the code path is
written to be reviewable and its assumptions documented in
`docs/DECISIONS.md` — but it has not been run.
