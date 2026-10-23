# Decision log

Architectural decisions, newest first within each phase. Each entry records what was decided,
what it rules out, and what would make it worth revisiting. Decisions are not revised in place:
a superseded decision keeps its entry and gains a note pointing at the one that replaced it.

---

## ADR-001 — Toolchain: AGP 9 with built-in Kotlin, Gradle 9.8.2, convention plugins in an included build

**Status:** accepted

**Context.** The project has sixteen modules that need identical Android, Kotlin, Compose and
Hilt configuration. Versions had to be resolved against live Maven metadata rather than from
memory, which is how the current set was arrived at: AGP 9.4.1, Gradle 9.8.2, Kotlin 2.4.21,
KSP 2.3.12, compileSdk/targetSdk 37, minSdk 29, NDK 28.2.13676358, Compose BOM 2026.09.00.

**Decision.** Share configuration through convention plugins (`parallax.android.library`,
`...application`, `...compose`, `...hilt`, `...room`, `...feature`) in an **included build**
called `build-logic`, not in `buildSrc`.

**Why not `buildSrc`.** `buildSrc`'s `implementation` dependencies land on the root build's
script classpath. With the Android and KSP plugins there, every module's
`alias(libs.plugins.android.library)` fails with *"plugin is already on the classpath with an
unknown version"*. An included build keeps the plugin classpath isolated. This cost a working
afternoon to diagnose, which is the reason it is written down.

**Consequences.**
- AGP 9 removed the `org.jetbrains.kotlin.android` plugin; Kotlin is configured through
  `kotlin { compilerOptions { } }` and the plugin is not applied anywhere.
- AGP 9 fails the build on a missing `consumerProguardFiles` entry, so every library module has
  a `consumer-rules.pro` even when it is empty but for a comment.
- `targetSdk` defaults to `compileSdk`, so only `compileSdk` is set.
- A Google Maven Central mirror is listed ahead of `mavenCentral()` in all four repository
  blocks, because Maven Central returns HTTP 429 to CI-shaped request bursts.

---

## ADR-002 — Reimplement the geometry stack instead of depending on OpenCV and Ceres

**Status:** accepted
**Supersedes nothing. Expanded by ADR-003 and ADR-004.**

**Context.** The reconstruction engine needs feature detection and description, two-view
geometry, triangulation, PnP, bundle adjustment, TSDF fusion, isosurface extraction, mesh
decimation and texturing. The obvious route is OpenCV plus Ceres.

**Decision.** Implement them, with Eigen (MPL-2.0, built with `EIGEN_MPL2_ONLY`) and zlib as
the only third-party dependencies.

**Why.**
1. **Licensing.** The brief permits MIT, BSD, Apache-2.0 and MPL-2.0 only. OpenCV's core is
   Apache-2.0, but `opencv_contrib` — where SIFT lived until recently — carries extra terms, and
   the usual dense-reconstruction companion, OpenMVS, is AGPL. Avoiding the question entirely is
   cheaper than auditing it per module per release.
2. **Size.** OpenCV adds roughly 15–25 MB per ABI even when stripped to the modules used. The
   whole native engine is under 2 MB per ABI.
3. **Host testability.** The engine builds and runs on the host with no Android SDK, no NDK and
   no device, so all 255 native tests run in CI in seconds against synthetic scenes with known
   ground truth. A pipeline that can only be tested on a phone is a pipeline that is not tested.

**Consequences.** Accuracy and robustness are ours to own, and ADR-003 records where that
currently costs us. Some algorithms were deliberately chosen for *verifiability* over raw
performance; see ADR-006.

---

## ADR-003 — ORB-style steered BRIEF rather than SIFT, with the wide-baseline limitation recorded

**Status:** accepted, with a known limitation

**Context.** Pose-less reconstruction (Import mode: a folder of photos, no ARCore poses) needs
features that match across wide baselines. SIFT is the standard answer. Implementing SIFT well —
scale-space extrema with sub-pixel refinement, orientation histograms, 128-D gradient
descriptors, and the matching heuristics that make it work — is a large, subtle piece of work,
and its patent-era baggage means most permissive implementations are recent.

**Decision.** Use FAST-9 keypoints with a Harris-style response for ranking, an intensity
centroid for orientation, and a steered BRIEF-256 descriptor matched by Hamming distance under a
ratio test, inside an adaptive RANSAC driver.

**Measured limitation.** On synthetic orbit sequences this registers reliably over short
baselines, and `PipelineTest` asserts the capability that actually holds (at least three frames
registered from a cold start with no poses) as a regression guard rather than asserting a number
the implementation does not reach. Widening the matching window raised the track count from 537
to 4,779 and a coplanarity guard plus local bundle adjustment after each registration improved
stability, but incremental growth still stalls earlier than a SIFT-based front end would: steered
BRIEF is markedly weaker at wide baselines, and DLT PnP is degenerate for coplanar point sets.

**What this means for the product.** Object, Room and Area modes use ARCore poses and are
unaffected; Import mode of a dense, well-overlapped capture works; Import mode of a sparse,
wide-baseline set will register a subset of frames. The UI says so rather than producing a
confidently wrong model.

**Revisit when.** A permissively licensed, compact SIFT or a learned local feature small enough
to run per frame on-device is available — or when Import mode is measured to be a common enough
path to justify writing one.

---

## ADR-004 — A purpose-built sparse Levenberg–Marquardt solver rather than Ceres

**Status:** accepted

**Context.** Bundle adjustment needs a sparse non-linear least-squares solver. Ceres is
Apache-2.0 (its SuiteSparse dependencies are not uniformly so) and is the industry default.

**Decision.** Implement Levenberg–Marquardt with an explicit Schur complement: eliminate the
point block, solve the reduced camera system densely, back-substitute for the points. Huber loss
on reprojection residuals. The scale gauge is fixed by projecting the solution back after every
step, because a monocular reconstruction is otherwise free to drift in scale.

**Why.** The problem shape here is narrow — one residual type, one parameterisation (SE(3)
tangent, translation first then rotation), a few hundred cameras at most. A general solver's
flexibility buys nothing and costs a dependency, a build-system integration and an unauditable
amount of binary size.

**Measured result.** Under 0.05 px RMSE on synthetic scenes.

**One behavioural note worth recording:** the first implementation reported "no cost-reducing
step found" as a failure. That is the *convergence* condition when the starting point is already
optimal, which happens often with ARCore poses as the initialisation. It is now treated as
success.

---

## ADR-005 — Metadata in SQLite, binary data on the filesystem

**Status:** accepted

**Context.** A single scan is tens to hundreds of megabytes: frames, depth maps, per-stage
checkpoints, meshes, atlases, point clouds, splats.

**Decision.** Room holds project metadata only. Everything binary lives under
`files/projects/<id>/` with a layout fixed as a contract between Kotlin and C++:
`manifest.json`, `frames/`, `depth/`, `checkpoints/`, `outputs/`, `exports/`, `thumbnail.jpg`.

**Consequences.**
- The native pipeline is handed a directory and reads and writes it directly. It never touches
  the database, never needs a JNI call per file, and can resume after a process death by looking
  at what is on disk.
- Two stores have to agree. `ProjectRepository` is the only writer of both, creating the
  directory before the row and deleting the row before the directory, so a crash leaves at worst
  an orphaned directory. `reconcile()` reclaims those at startup.
- The `status` column is duplicated: a `status_kind` discriminator for SQL to filter and sort on,
  alongside the serialised status. Without it every library query would have to deserialise every
  row.

---

## ADR-006 — Marching tetrahedra, not marching cubes

**Status:** accepted

**Context.** The TSDF volume needs an isosurface extractor.

**Decision.** Marching tetrahedra over the Freudenthal (Kuhn) decomposition of each cube into six
tetrahedra sharing the main diagonal.

**Why.** Marching cubes needs a 256-entry case table that cannot be verified by reading it; an
error in one entry produces holes in a minority of configurations, which is exactly the kind of
defect that survives testing and reaches users. With four corners there are three distinct cases
up to complement symmetry, and each follows from the sign pattern by inspection. The six-tetra
decomposition is also crack-free between neighbouring cubes without the orientation alternation a
five-tetra split requires.

**Cost.** Roughly twice the triangles of marching cubes for the same field. Decimation removes
them in the next stage anyway, and the surface is identical to within the field's own resolution.

**A bug this decision did not prevent, and how it was caught.** Deriving triangle winding from
corner-index order inverted about 40% of triangles, because the six tetrahedra do not all have
the same handedness. Winding is now decided geometrically, from the direction between the
centroids of the inside and outside corners. The first attempt at that fix negated the outward
direction *and* swapped two vertices, which made the sign test pass again and recursed forever;
the orientation is now decided once, without recursion.

---

## ADR-007 — Floor plans from the floor plane, not from wall enclosure

**Status:** accepted
**Supersedes:** the flood-fill approach used until the room tests were written.

**Context.** A room's floor plan has to be derived from a scan that is never closed: doorways are
open, and walls have gaps wherever the capture missed.

**First approach, and why it failed.** Rasterise walls into an occupancy grid and flood-fill the
interior. A single one-cell gap in a wall leaks the fill into the entire exterior. On the
4.2 × 3.1 m test room this produced an 8% area error.

**Decision.** Derive the interior from the points that lie on the detected **floor plane**, then
close morphologically, fill holes, keep the largest connected component, simplify with
Douglas–Peucker, and regularise by snapping *lines* and rebuilding vertices as their
intersections.

**Measured result.** 13.00 m² against a true 13.02 m², a 0.15% error.

**Two further bugs worth recording.**
- Snapping each edge by rotating it about its own midpoint clobbered the vertices it shared with
  its neighbours, destroying one of the two rooms in the two-room test. Snapping the infinite
  lines and recomputing the corners as intersections is what fixes it.
- Openings (doors, windows) cannot be detected from the 2D grid at all: a window has wall below
  it, so it is occupied in plan. Detection uses a 3D vertical height-bin profile of near-vertical
  surfaces only — the floor and ceiling have to be excluded, or the bottom bin is occupied
  everywhere and no doorway ever registers.

---

## ADR-008 — Per-triangle texture atlas with Gauss–Seidel seam levelling

**Status:** accepted

**Context.** The textured mesh needs a UV parameterisation and an atlas. The quality answer is
a chart-based parameterisation (UVAtlas, xatlas) with Poisson blending across chart boundaries.

**Decision.** Give every triangle its own square patch in a shelf-packed atlas, choose the source
view per triangle by a score of sharpness × incidence × distance with depth-map visibility
testing, and level the brightness discontinuities with Gauss–Seidel iterations over the
shared-edge graph, followed by inpainting of unassigned texels.

**Why.** Chart-based parameterisation is a research-grade amount of code, and its failure mode
on scan meshes — which are non-manifold in places and have thousands of tiny boundary loops — is
charts that will not pack. Per-triangle patches always pack, always parameterise, and let the
view choice be per-triangle rather than per-chart, which is where most of the sharpness comes
from.

**Costs, and what was done about them.**
- Gutters are 50% overhead on a 4-texel patch. Padding is now `max(1, min(requested, size/4))`.
- The atlas may be too small for every triangle. Rather than failing the stage, it textures as
  many as fit, reports `triangles_dropped_for_space`, and the decimation target upstream is
  capped at `(atlas/6)² × 0.7` so this is rare.
- Patch size is solved analytically (`rows = ceil(sqrt(textured))`) rather than by iterative
  shrinking, which used to give up before finding a size that fits.

---

## ADR-009 — Gaussian splat training on the GPU via Vulkan compute, with an explicit capability gate

**Status:** accepted

**Context.** 3D Gaussian splatting needs tens of thousands of gradient-descent iterations over
millions of Gaussians. That is a GPU workload.

**Decision.** Train in Vulkan compute shaders, gated on `supportsVulkanCompute` **and** at least
6 GB of RAM (`DeviceCapabilities.canTrainSplats`). Devices that fail the gate get meshes and
point clouds, and the settings screen states the reason rather than hiding the feature.

**Why a gate rather than a CPU fallback.** A CPU fallback exists in principle and is useless in
practice: the arithmetic is thousands of times too slow to finish before the battery does.
Offering it would be a worse experience than saying plainly that the device cannot do it.

**Consequence.** Splat training is the one part of the engine that cannot be exercised on the
host, and that is recorded in `PROGRESS.md` under "verification boundaries" rather than papered
over with a mock.

---

## ADR-010 — Every export format written by hand, and the `.spz` interop boundary stated

**Status:** accepted

**Context.** The brief asks for GLB, glTF, OBJ+MTL, USDZ, PLY, 3DGS PLY, `.splat`, `.spz`, STL,
floor plans as PNG/PDF/SVG, and a self-contained HTML viewer. It also asks for a minimal
compliant USDZ writer if no permissively licensed library exists.

**Decision.** Implement all of them, plus the JSON parser, PNG encoder (zlib with adaptive row
filters) and ZIP writer they need, in the native engine. The ZIP writer supports 64-byte entry
alignment because USDZ requires it.

**Why.** The formats that matter are specified and stable, and a writer is far less code than a
reader. Round-trip tests validate each one by reading back what was written and comparing
geometry, which is a stronger check than "the file opened in a viewer".

**The honest boundary.** `.spz` v2 is written to the published layout with its quantisation
constants documented in `GaussianCloud.cpp`, and it round-trips through our own reader. It has
**not** been verified against the reference implementation's decoder, because doing so needs that
decoder, and the test suite is deliberately dependency-free. The limitation is stated in the
exporter's comment and in `PROGRESS.md` rather than being presented as verified interop.

---

## ADR-011 — Coarse-grained JNI with opaque handles

**Status:** accepted

**Context.** Kotlin has to drive a C++ pipeline that runs for minutes, reports progress, and must
be cancellable.

**Decision.** JNI calls are whole stages, whole files or whole meshes — never per-vertex or
per-pixel. Native objects are reached through an opaque `Long` handle into a registry guarded by
a mutex, not through a raw pointer cast to a `jlong`. Per-frame camera data crosses as a direct
`ByteBuffer`.

**Why handles rather than pointers.** A pointer that survives into a saved `Bundle` and is
restored after process death is a use-after-free with the attacker-free appearance of a crash in
unrelated code. A handle is just an integer: a stale one fails a lookup and returns an error.

**Why the registry mutex is released while a stage runs.** Otherwise a cancellation request —
which arrives on another thread and needs the registry to find the pipeline — would block until
the stage it is trying to cancel has finished.

---

## ADR-012 — Material You is opt-in, not the default

**Status:** accepted

**Context.** Material 3 on Android 12+ can derive the whole palette from the user's wallpaper.

**Decision.** Ship the dark graphite and electric-cyan palette as the default and offer dynamic
colour as a setting that is off until switched on.

**Why.** The single accent is used to mean something specific — live capture, coverage, the
current measurement — and the design depends on it being the only saturated colour on a dark
field. A wallpaper-derived palette can produce a low-contrast accent against a mid-grey surface,
which would make the capture overlay harder to read in exactly the conditions (bright sunlight,
one-handed) where it matters most. Users who prefer system theming can still have it.
