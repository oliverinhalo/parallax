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

## Remaining phases

- [ ] **Phase 3 — Exporters**: GLB/glTF, OBJ+MTL, PLY, STL (watertight), USDZ,
      3DGS PLY/.splat/.spz, floor plan SVG/PDF/PNG, self-contained HTML viewer
- [ ] **Phase 4 — Data layer**: Room schema, project storage, `.parallax`
      bundles, library screen
- [ ] **Phase 5 — Design system**: tokens, theme, components, motion, icon
- [ ] **Phase 6 — Capture**: ARCore + CameraX, 4 modes, live guidance,
      coverage visualisation, device capability detection
- [ ] **Phase 7 — Depth & ML**: sensor depth, monocular depth (LiteRT),
      scale alignment, segmentation
- [ ] **Phase 8 — Texturing**: view selection, atlas packing, seam levelling
- [ ] **Phase 9 — Gaussian splatting**: Vulkan training + renderer, tiers
- [ ] **Phase 10 — Processing system**: WorkManager queue, foreground service,
      checkpoint/resume, progress and ETA
- [ ] **Phase 11 — Viewer**: Filament, render modes, measurement, editing,
      annotations, walk mode
- [ ] **Phase 12 — Room features**: plane/wall/opening extraction, floor plan
- [ ] **Phase 13 — AR placement**
- [ ] **Phase 14 — Polish & hardening**: onboarding, settings, storage
      dashboard, accessibility, benchmarks, release build, docs

---

## Verification boundaries

Stated plainly so nothing here is mistaken for more than it is.

**Verified by tests that run in this repo:** everything in Phase 2, plus the
Gradle build and APK packaging in Phase 1.

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
