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

## Phase 5 — App shell, library, onboarding, settings ✅

157 JVM/Robolectric tests across six modules, all passing.

- **`:app`**: Hilt application, `MainActivity` that holds the splash until the
  stored theme is known (drawing with defaults first flashes the wrong theme on
  every cold start), type-safe navigation graph, edge-to-edge with transparent
  bars, and the adaptive icon — the parallax motif, two offset outlines of the
  same cube, with a themed monochrome layer for Android 13+.
- **No `INTERNET` permission.** The on-device promise is enforced by the
  platform rather than asserted in the README.
- **`AndroidDeviceCapabilityProvider`**: ARCore availability, Depth API
  pre-check, time-of-flight detection, Vulkan *compute* (not merely a Vulkan
  version — some devices report one with no compute queue), RAM, cores, camera
  resolutions. Every probe degrades to the conservative answer on failure,
  because an OEM returning null for a stream configuration map must not take the
  app down at startup.
- **`:feature:library`**: adaptive grid with status badges and per-mode
  placeholders, search over names and notes, folder/mode/favourite filters,
  multi-select with bulk delete and move, rename, `.parallax` import through the
  Storage Access Framework, and thumbnails decoded off the main thread at a
  bounded size.
- **`:feature:onboarding`**: four pages (promise, modes, technique, permission)
  with the camera permission asked in context, and a permanent denial
  distinguished from a refusal so the UI stops offering a button Android will
  ignore.
- **`:feature:settings`**: storage dashboard with a stacked bar and legend read
  from the filesystem rather than the database's cached column, frame
  reclamation that refuses to touch an unprocessed capture, every preference,
  the device's limitations spelled out, and an in-app licences screen.
- **Share staging** lives in `files/shared`, exposed through a `FileProvider`
  path of its own. A provider rooted at `projects/` to reach each project's
  `exports/` would hand any app receiving a share URI a path into every frame,
  checkpoint and database file.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| Kotlin backend crash in `:core:model` | `AssertionError: SyntheticAccessorLowering should not attempt to modify other files`, reported as an error but with the build still passing | Private companion constants inside an enum class; moved to file scope |
| Robolectric could not start under JDK 17+ | Every Robolectric test failed in application setup | Needs `--add-exports java.base/jdk.internal.access` for its file-descriptor interception |
| ViewModel tests were non-deterministic | State read before a write had landed | Room's default executors and DataStore's default scope are real background threads; both are now pinned to the test scheduler (`MainDispatcherRule`, `DirectExecutor`) |
| `assembleRelease` failed two ways | Missing ProGuard file, then a signing error on a build meant to be unsigned | AGP 9 fails on a missing `proguard-rules.pro`, and an empty signing config still runs `validateSigningRelease` |

---

## Phase 6 — Capture ✅

83 JVM tests on this module alone, and 11 more on the native JPEG decoder it
needed.

**The logic, which is all testable without a device.** The parts that decide
what happens are plain classes behind seams, so the behaviour that matters runs
in the test suite rather than only on a phone:

- **`CoverageTracker`** — a sphere of bins around the subject, filling over
  several frames rather than flipping on first sighting, with the largest gap
  classified as underside, top, back or side.
- **`GuidanceEngine`** — a pure function from conditions to the single most
  useful prompt, plus **`GuidanceSession`** for the hysteresis that makes it
  readable: a prompt holds for 1.4 s, and only a higher-priority one interrupts.
- **`KeyframeSelector`** — a frame earns its place by adding parallax, rotation,
  coverage or sharpness; a perceptual hash is the backstop for the case poses
  cannot catch.
- **`CaptureBudget`** — per-mode capture rates scaled by thermal state, keyframe
  ceilings from RAM and preset, and storage projections.

**The device layer, kept thin.** `ArCaptureSession` wraps ARCore and classifies
every failure into something the user can act on; `CameraBackgroundRenderer` is
one shader and a quad; `FrameRecorder` converts the camera's YUV planes straight
to JPEG and writes ARCore's packed 16-bit depth as the engine's raw float
format; `MediaImporter` builds a pose-less capture from gallery photos or video,
reading intrinsics from EXIF where they exist.

**A baseline JPEG decoder in the native engine**, because capture writes JPEG
(250 MB a scan against 1.3 GB of PNG) and the pipeline has to read it. Validated
against libjpeg-turbo's own output on eight fixtures covering 4:2:0, 4:2:2,
4:4:4, greyscale, restart markers, flat colour and non-MCU-aligned dimensions —
regenerate with `tools/make_jpeg_fixtures.py`.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| Chroma sampled at the wrong position | Mean error of 4.4 code values against libjpeg-turbo, worst case 103 | A 4:2:0 chroma sample's centre is half a pixel right and down of the pixel it starts at; nearest-neighbour replication shifts both chroma planes, which shows as colour fringing on every edge |
| Azimuth gaps masked by the poles | "You missed the back" never fired on a scan missing a whole side | The column average ran over all elevations, so top and bottom rows covered all the way round kept every column above the threshold |
| The first keyframe was rate-limited | A short scan recorded nothing at all | The throttle measured from a zero initialiser, so the first frame had to wait out the interval |
| Restarting a capture kept the old frames | Two half-orbits from different starting points reconstructed as one project, producing a worse model than either alone | `start()` reset its own counters but not the recorder's |
| `LowStorage` ranked below `HoldStill` | A nearly full disk was reported only once tracking was stable | Tracking recovers; storage does not. Re-ranked, with the reasoning recorded on the enum |

---

## Phase 7 — Depth, ML and the processing system ✅

Phases 7 and 9 of the original plan landed together, because they are one
question seen from two ends: *where does depth come from on a device that has
none*, and *what drives the pipeline that consumes it*. Splitting them would
have meant shipping a depth path with nothing to run it.

**Depth, in three tiers, chosen per device.** ARCore's depth API when the
device provides it; otherwise monocular depth from Depth Anything V2 Small
(Apache-2.0 — the Base and Large weights are CC-BY-NC and are not used) run
through LiteRT; and below that, native multi-view stereo. The third tier is
what makes the app work on *any* device in the minSdk 29 range, including one
with no depth sensor and no model on disk.

`MultiViewStereo` is a plane sweep: for each keyframe, a handful of neighbours
are chosen by baseline-to-depth ratio and view alignment, hypotheses are spaced
uniformly in inverse depth, scored by normalised cross-correlation, filtered
for geometric consistency between views and median-smoothed. Untextured regions
are rejected rather than guessed at, by a minimum patch-norm threshold — a
blank wall returns no depth instead of a confident wrong answer.

`DepthScaleAligner` fits monocular relative depth to metric scale against the
sparse points from SfM. The fit is affine **in inverse depth**, which is where
the relationship is actually linear, by RANSAC over two-point minimal samples
with a least-squares refit on the inliers and a deterministic 48-bit LCG so the
same capture always produces the same model.

**The processing system.** `ReconstructionRunner` drives the ten stages and
translates the engine's JSON contract; `ReconstructionWorker` is one
CoroutineWorker that drains the whole queue rather than one worker per project,
because reconstruction is memory- and thermally-bound and two at once is slower
than two in sequence. Everything that survives a kill is on disk: the queue in
the database, each stage checkpointed by the engine. Progress arrives from the
native thread many times a second and is coalesced onto a 400 ms timer, so the
database, the library, the processing screen and the notification all read one
source of truth and a twenty-minute run does a few thousand writes rather than
a few million.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| Missing kotlinx-serialization plugin | `SerializationException` at the first stage of every run | `:feature:processing` declared the dependency but not the compiler plugin, so the `@Serializable` contract classes had no generated serialiser. A compile-clean module that cannot run a single job |
| Expedited work rejected at enqueue | `IllegalArgumentException: Expedited jobs only support network and storage constraints` | Reconstruction must not run on a hot or nearly flat battery, and those constraints are incompatible with expedited work. Expedited was dropped; the user-visible foreground notification is what the queue needs, not the quota |
| Config defaults silently omitted | The runner and the engine disagreed on `truncationMultiplier` | `encodeDefaults = false` dropped any field left at its default from the JSON the engine parses |
| Stage completion written per tick | A resumed job restarted its stage | The completed-stage counter is what makes resume work; it is now written when the stage ordinal advances rather than on the progress timer |

---

## Phase 8 — Gaussian splatting ✅

**The mathematics is written twice, and the version that can be tested is the
one that defines the other.** `SplatRaster.cpp` is the EWA projection, the
tile-binned front-to-back compositing, the analytic backward pass for every
parameter, SSIM and the photometric loss. `SplatTrainer.cpp` is Adam and the
schedule; `SplatDensify.cpp` is adaptive density control. Sixteen compute
shaders under `src/splat/vulkan/shaders/` implement the same equations for the
device, and each names its host counterpart at the top of the file.

**Every gradient is checked against central differences**, scored with a
linear functional of the pixels rather than L1, so the objective is smooth and
the check tests the rasteriser rather than the loss. The trainer is checked by
holding a camera out of the training set and requiring the error on that
unseen view to halve — a claim about the geometry, not about memorising the
training images. The whole stage is then exercised through the real pipeline on
a synthetic orbit, from point cloud to a loadable `splat.ply`.

**The GPU path is the shipping path; the CPU path is the floor.**
`trainSplatsOnGpu` returns false rather than failing when Vulkan is missing,
too old, out of memory or lost, and the pipeline falls back to the CPU trainer
on a fifth of the iterations with a third of the gaussians. See
`docs/DECISIONS.md` (ADR-009) for why that replaced the original "no fallback"
decision, and for what is and is not verified about the shaders.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| The SSIM blur clamped at the image edge | The corner of the gradient was 40% wrong and nothing else was | The gradient applies the same blur as its own adjoint, which only holds for a symmetric kernel over a zero-padded domain. Clamping folds the out-of-bounds weight onto the border, whose adjoint is a different operator |
| The quaternion gradient ignored normalisation | Rotations trained to the wrong shape, for some gaussians and not others | The forward pass normalises the quaternion; the backward pass did not push the gradient back through that, leaving the component along the quaternion itself wrong |
| The clamped-frustum derivative was double-counted | Gaussians near the edge of the frame drifted in depth | Once a coordinate is clamped into the widened frustum the projection Jacobian's z-derivative halves, and the unclamped expression was used for both cases |
| The tile-culling radius was floored at 0.1 | Near-isotropic splats pulled in tiles they could not reach | A guard meant to keep a square root non-negative had been raised to a comfortable-looking constant, which inflated the eigenvalue spread of every circular splat |
| A preset that trains no splats crashed the budget | `Cannot coerce value to an empty range` on the Fast preset | The gaussian budget clamped the device's affordable count into the preset's range without first asking whether the preset wanted splats at all |

---

## Phase 10 — Viewer ✅

**The device layer is one file.** `ModelSurface` owns every Filament call --
engine, swap chain, camera, lights, asset loading -- and nothing else in the
module knows Filament exists. Everything above it works in terms of a camera
position and a GLB on disk, which is what makes the rest of the viewer
testable: the camera feel, the picking, the measurement arithmetic, the edit
algebra and the whole interaction state machine run on a JVM.

**The camera is pure state and pure transitions.** Orbit with clamped pitch,
distance-scaled panning and frame-rate-independent inertia; walk mode confined
to the scan's own bounds with an adjustable eye height. Both are data classes
with no dependencies, and both are tested against what they are for -- that
framing a box puts every corner inside the frustum, that a flick decays to
rest, that a long frame does not overshoot into a reversal.

**Picking, measuring and editing.** A uniform grid over the triangles turns a
tap into a point on the surface; measurements are distance, height and area,
with area by Newell's method because the points a finger taps on a scanned
floor are never quite coplanar. Edits are stored as intent -- crop to this box,
rescale so that this measurement is really 2.4 m -- and folded into one
transform and one keep test, so the scan is never rewritten and any edit can be
undone months later.

**Points and splats go through the same loader as the mesh**, by being converted
to triangle geometry in the native engine and written out as GLB. That is
ADR-013, and it is what let the hardest part of the rendering be unit-tested.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| The levelling rotation turned the floor the wrong way | Re-levelling a tilted scan tipped it further over instead of flat | Rodrigues' formula about `Y x n` rather than `n x Y`. The angle was right, so the model rotated by exactly the correct amount in exactly the wrong direction |
| A lasso cut kept what was behind the camera | Geometry the user could not see was being deleted along with what they selected | The test's own projection matrix had no perspective in it, which is what exposed that the behind-the-camera rejection needed `w` to carry depth |
| The brightness slider was unbounded | Nothing yet, but a saved value of 40 stops would have rendered pure white | The clamp was written into the UI rather than the state |

---

## Phase 11 — Export, AR placement and the offline viewer ✅

**Thirteen export formats, one screen.** Every writer is in the native engine,
so the Kotlin side is a translator: it picks the file name, chooses the right
engine call, and turns the engine's answer into a file or a message someone can
act on. Formats a scan cannot produce are listed separately rather than hidden,
because a user hunting for USDZ on a point-cloud-only scan needs to know there
is no mesh, not be left wondering whether the app has it.

Exports are written into the project first and copied out afterwards. A share
sheet needs a file that outlives the call, a save dialog needs a source to copy
from, and neither should be able to leave a truncated file in someone's
Downloads. Sharing stages a copy in the one directory the FileProvider exposes,
so a share grant cannot reach the frames or the checkpoints.

**The offline HTML viewer is self-contained, and tested for it.** Geometry goes
in as base64 typed arrays, the texture as an embedded PNG, and the renderer is
a few hundred lines of WebGL 1 inlined beside them -- no CDN, no second file, no
`fetch`. A test asserts the absence of `http`, `script src`, `<link>`, `fetch`
and `XMLHttpRequest`, because "self-contained" is the whole feature and it would
be easy to regress by one convenient import.

**AR placement** is the placement arithmetic as pure state -- where the model
sits, how big, which way round -- with ARCore and GLES behind it. A room scan
placed life size will not fit in the room it is being shown in, so anything
over head height goes down as a tabletop model instead.

The camera-background shader that capture and AR placement both need moved into
`:core:ar`, which exists for exactly that one file.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| The scan's name went into the HTML unescaped | A scan called `<script>…` would have run that script in the browser of whoever it was sent to | Exported HTML is a file other people open, and the title comes straight from user input. Title and facts are now escaped, and a test asserts it |
| A texture was embedded for a mesh with no UVs | Hundreds of kilobytes of base64 atlas that nothing could sample | The texture was embedded whenever one was offered, rather than only when the geometry could use it |
| An unnamed scan produced a file called `.glb` | A file with no name, which some file managers hide | The name sanitiser could reduce a name to nothing, with no fallback |

---

## Phase 12 — Polish, benchmarks and CI ✅

**Three CI jobs.** The native engine built and tested on the host with its
microbenchmarks recorded as an artifact; the Android build with unit tests,
Lint, detekt and R8; and the screenshot comparison. The benchmark numbers are
recorded rather than asserted on, because a shared runner is too noisy to fail
a build over and the point is that a tenfold regression appears in the log of
the run that caused it.

**Benchmarks at both levels.** Macrobenchmarks for cold, warm and as-installed
startup, and for frame timing while flinging the library. A native binary times
the ten hot paths; on this machine multi-view stereo costs two orders of
magnitude more per megapixel than anything else, which is what the quality
presets are really trading away.

**Screenshot tests, including at twice the font size.** One gallery of every
component rather than one test per component: the thing worth catching is a
change to a shared token, and a single image shows all of it at once. The
large-font variant exists because that is the accessibility regression easiest
to introduce and hardest to notice — and it immediately found two.

**Accessibility.** Minimum touch targets of 48dp on every interactive
component, content descriptions on every icon-only control and every slider,
layouts that wrap rather than clip at large font scales, and a coverage ramp
that varies lightness within one hue rather than running red to green — which
is the obvious choice and unreadable to the eight percent of men with a
red-green deficiency.

### Bugs this phase's tests caught

| Bug | Symptom | Cause |
|---|---|---|
| The theme never painted its own background | Dark-on-light text on every screen without its own surface, for anyone running the app dark on a light system | The activity's `windowBackground` follows the *system* dark mode while the theme follows the user's setting. The two agree most of the time, which is why it would have shipped |
| The export screen's unit chips could not all be reached | "Feet" was off the right edge at any font scale | Five chip labels in a plain `Row` on a 411dp phone |
| The badge row ran off the screen at 2x font scale | "Success" invisible, "Danger" squeezed to one character wide | Same cause, only visible at a large font scale |
| A dangling reference in the benchmark harness | Segfault partway through the run | A `[&]` lambda over a block-scoped C++ fixture, called after the block ended |

---

## Verification boundaries

Stated plainly so nothing here is mistaken for more than it is.

**Verified by tests that run in this repo:** 337 native gtest assertions and
386 JVM/Robolectric tests, plus the Gradle build, detekt, Android Lint, R8
release minification, APK packaging and screenshot comparison. All of it runs
on every push through `.github/workflows/ci.yml`.

The capture *logic* is covered — coverage accumulation, guidance priority and
hysteresis, keyframe selection, rate limiting, thermal throttling, the summary
and the import path. What is not covered is the ARCore session itself, the GL
camera background and the YUV-to-JPEG conversion, which need a camera.

**Cannot be verified in this environment** (no physical device, no camera, no
GPU, no ARCore service):

- ARCore pose/depth/plane behaviour and anything built on it
- Camera capture and real camera intrinsics
- The Vulkan compute shaders. They are compiled to SPIR-V as part of every
  build, on the host as well as for Android, so a shader that stops compiling
  breaks CI; but no shader here has ever been executed. Their arithmetic is a
  transcription of code that is gradient-checked, and a device that cannot run
  them falls back to that code rather than losing the feature
- Filament rendering output. `ModelSurface` is the one file that cannot be
  tested here, which is why it is the only file in the viewer that touches
  Filament and why a renderer that refuses to start is a state the screen
  handles rather than a crash
- LiteRT model inference with GPU/NNAPI delegates
- Real-device frame rates, thermal throttling and memory behaviour

For these, the engine's host-testable parts are tested against synthetic data
and the device-only layers are kept as thin as possible, so that the logic that
*can* be tested is tested. Where an algorithm is device-only, the code path is
written to be reviewable and its assumptions documented in
`docs/DECISIONS.md` — but it has not been run.

---

## Phase 13 — The app would not start

The first install on a real device (Galaxy S26 Ultra, One UI 9) died instantly on launch. No
test caught it, because `:app` had no unit tests at all: every module below it was covered, and
nothing had ever built the whole graph and composed the navigation host on top of it.

### Bugs this phase's tests caught

| Symptom | Cause |
| --- | --- |
| The app closed immediately on launch, on every device | `:app` never applied the kotlinx-serialization *compiler plugin*. Type-safe navigation looks up a generated serialiser for each destination, so the first composition threw `SerializationException: Serializer for class 'Onboarding' is not found` before a frame was drawn. The module compiled cleanly: the plugin is separate from the dependency, which is the trap already written down in `CLAUDE.md` and still walked into. |
| Opening a finished model would have thrown the same way | `:feature:viewer` was missing the same plugin. Its `@Serializable` type parses the native preview builder's answer, which is behind a JNI call that cannot run in CI, so nothing had ever resolved that serialiser. |
| The crash reporter killed the Gradle test worker | Its handler called `exitProcess` after delegating. On a device the platform's handler already ends the process; under Robolectric there is no platform handler and an uncaught exception is a test result, not a death. It now delegates and does not exit. |

The first of those is the whole reason the phase exists. The second was found by looking for
the same mistake elsewhere rather than by the crash, which is the more useful half.

### What is new

- `:app` has unit tests: the dependency graph builds, WorkManager's configuration is supplied,
  nothing in our own code throws on the way to the first frame, and every navigation route
  resolves a serialiser. The last of those fails if the plugin is removed and names the route.
- An offline crash screen (ADR-016): a handler installed before Hilt builds anything, writing
  a report and showing it in a separate process with a Share button. No network, as everywhere
  else.

### Verification boundaries

The startup test runs under Robolectric, which cannot emulate Play's install service: ARCore's
availability check binds it and gets a null binder back, so the first-frame test ignores frames
outside our own packages. A failure inside ARCore itself on a real device would not be caught
here.

---

## Phase 14 — Switching capture mode took the process down

Reported from a device: Object mode worked, switching to any other mode closed the app, and it
would not start again until it was reinstalled. Two separate bugs, one of them mine from the
phase before.

### Bugs this phase's tests caught

| Symptom | Cause |
| --- | --- |
| Switching capture mode closed the app with no dialog and no crash report | `ArViewfinder` remembered its renderer keyed on the capture mode, and each renderer owns an `ArCaptureSession`. ARCore permits one session per process, so a mode switch built a second one beside the live one and the process died in native code -- where no Kotlin handler can see it. The renderer never used the mode at all. It also rebuilt when `preferDepth` flipped as the capability check came back, which is the same crash on a slower path. |
| Once it had happened, the app would not open again until it was reinstalled | The crash report was cleared after the crash screen was shown. When the screen did not appear, the report stayed, so every launch stopped to show a screen that never arrived and then finished the activity. Reinstalling was the only way to clear the file. The report is now consumed by the attempt, and the app carries on behind the screen rather than finishing. |
| The renderer would have gone on calling the first composition's callbacks | Fixing the key exposed it: a renderer built once captures the lambdas it was built with, and `onTick` is handed a new one on every recomposition. They are read through `rememberUpdatedState` now. |

### Verification boundaries

That two ARCore sessions kill the process cannot be reproduced here: it is native, and there is
no ARCore in CI. What the tests do cover is that the capture screen composes in every mode and
recomposes between them, and that a stored crash report is consumed by the attempt to show it.
The single-session rule itself is held by one `remember` with no key and a comment saying why --
which is weaker than a test, and worth knowing.

