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

## ADR-009 — Gaussian splat training in Vulkan compute, with a CPU reference that is both the specification and the fallback

**Status:** accepted (supersedes the original form of this decision, which gated splats on Vulkan
and offered no fallback)

**Context.** 3D Gaussian splatting needs thousands of gradient-descent iterations over hundreds of
thousands of gaussians. That is a GPU workload. But no part of it can be executed in this
repository's CI: there is no GPU, no Vulkan loader and no driver, so a Vulkan-only implementation
would ship having never run.

**Decision.** Three things, in this order of authority.

1. **The CPU implementation is the specification.** `SplatRaster.cpp` holds the EWA projection,
   the tile-binned compositing, the analytic backward pass, SSIM and the loss; `SplatTrainer.cpp`
   holds Adam and the schedule; `SplatDensify.cpp` holds adaptive density control. Every gradient
   is checked against central differences, and the trainer is checked by holding a camera out of
   the training set and requiring the error on that unseen view to halve. This is what defines
   correct behaviour.
2. **The Vulkan compute path is the shipping path.** Sixteen compute shaders under
   `src/splat/vulkan/shaders/` implement the same equations: projection, a four-bit LSD radix sort
   over packed (tile, depth) keys, two-level prefix sums, forward and backward rasterisation with
   compare-and-swap float atomics, the SSIM chain, and Adam. They are compiled to SPIR-V by glslc
   as part of the build — on the host as well as for Android — so a shader that stops compiling
   breaks CI rather than a device.
3. **A device that cannot run the GPU path still gets splats.** `trainSplatsOnGpu` returns false
   rather than failing when Vulkan is missing, too old, out of memory or lost, and the pipeline
   falls back to the CPU trainer with the schedule cut to a fifth of the iterations, a third of
   the gaussians and one harmonic band. The settings screen says which path a device will take.

**Why the fallback, when the original decision rejected one.** The original reasoning was that a
CPU fallback is "thousands of times too slow to finish before the battery does". That was an
estimate made before the reference existed. With a tile-binned rasteriser, 512-pixel training
views and a schedule sized to the device, a reduced run is minutes rather than hours — slower and
coarser than the GPU, but a result rather than a refusal. It also means that a defect in the
untested GPU path degrades the experience instead of removing the feature.

**The honest boundary.** The compute shaders are compile-verified and their arithmetic is a
line-by-line transcription of code that is gradient-checked, but they have not been executed. They
cannot be, here. `PROGRESS.md` records this under "verification boundaries" rather than counting
the shaders as tested, and the fallback exists so that the first device to run them is not also
the first device to be stuck without splats.

**Consequences.**

* Two implementations of the same mathematics have to be changed together. The shader headers name
  their host counterpart in a comment at the top of each file for exactly that reason.
* The GPU path caps the model at 262,144 gaussians, which is what keeps every prefix sum to two
  levels. That is also roughly what a phone can hold.
* The sort key packs fourteen bits of tile and eighteen of quantised depth into one 32-bit word,
  halving the radix passes. It bounds a render to 16,383 tiles, which the host checks.

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

## ADR-013 — Points and splats are drawn as triangle geometry, built natively

**Status:** accepted

**Context.** A point cloud and a Gaussian splat field both want their own shader: points as
screen-facing sprites, gaussians as projected ellipses with an analytic falloff. In Filament a
custom material has to be compiled by `matc` at build time, which means another tool in the chain
and a shader whose output could not be checked anywhere in this repository.

**Decision.** The viewer has exactly one way to draw something: load a GLB. Point clouds become
one small disc per point facing that point's normal; splats become two triangles per gaussian
spanning its two widest axes, textured with a baked radial alpha ramp. Both are built by
`buildPointCloudPreview` and `buildSplatPreview` in the native engine, written out through the
same tested GLB writer the exporters use, and cached beside the project's outputs.

**Why.** It moves the whole of the interesting work — disc sizing from local point spacing,
choosing which of a gaussian's axes to keep, the falloff ramp — into code that has unit tests,
and leaves the renderer with nothing to get wrong. It also means the cached preview is an
ordinary glTF file that any other viewer can open, which made the round-trip test possible.

**The trade-off, stated plainly.** Splats drawn this way are a good likeness rather than a correct
rasterisation. The quad is a planar slice of the gaussian rather than its projected covariance, so
a very elongated splat looks slightly wrong edge-on; and the blend order is fixed when the file is
written (by descending opacity) rather than sorted per frame, so a dense field seen from an
unlucky angle can show ordering artefacts. For inspecting a scan this is the right trade; for
publishing one, the exported `.ply`, `.splat` and `.spz` carry the real gaussians.

---

## ADR-014 — Lighting presets from directional lights, not an environment map

**Status:** accepted

**Context.** Physically based rendering looks best with an image-based light, and Filament's
`cmgen` turns an HDR panorama into the cubemaps it needs. The brief asks for lighting presets.

**Decision.** The four presets are built from two or three directional lights each, with
different intensities and colour temperatures, plus a plain dark skybox.

**Why.** An IBL would need `cmgen` in the build and several megabytes of cubemap in the APK, in
an app whose whole premise is that it needs no downloads and works in airplane mode. Three
directional lights give four presets that genuinely look different from each other, which is what
the setting is for.

**Consequence.** "Environment" is a warm overhead light against a cool bounce from below rather
than a real sky, and reflections on a scanned surface are approximate. Scanned surfaces are
diffuse and have little to reflect, so this is less of a loss than it would be for a product
render.

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

---

## ADR-015 — Releases are signed with repository secrets, and fall back to a throwaway key

**Status:** accepted

**Context.** The APK has to be installable from the GitHub Releases page. Android will not
install an unsigned APK, and `app/build.gradle.kts` deliberately creates no signing config when
the environment variables are absent, so `assembleRelease` on CI produces something nobody can
use.

**Decision.** A tag-triggered `release` workflow decodes a keystore from
`PARALLAX_KEYSTORE_BASE64` and signs with it. When that secret is not set, it generates a 4096-bit
RSA key in the runner's temporary directory, signs with that, and discards it — then says so in a
workflow warning and in the release notes.

**Why not fail instead.** A release job that refuses to produce anything until four secrets exist
means the first release of a fresh clone is a configuration exercise. The fallback makes the
common case — install it on a phone and try it — work immediately.

**What it costs.** An APK signed with a throwaway key cannot be upgraded in place: the next
release has a different signature, so Android rejects the update and the user has to uninstall
first, losing their scans. That is a genuinely bad outcome, which is why the fallback is loud in
two places rather than silent. It is a stopgap for trying the app, not a way to ship it; anything
beyond that needs the secrets set, and the keystore kept somewhere it cannot be lost, because
losing it has the same consequence permanently.

**Why not commit an encrypted keystore.** The passphrase would have to live somewhere too, and a
repository that contains both is a repository that contains the key.

---

## ADR-016 — An offline crash screen in its own process

**Status:** accepted

**Context.** Parallax makes no network requests, so there is no crash reporting service and
never will be. A user whose app dies on launch has nothing to send and no way to look: the
trace exists only in logcat, which needs a computer and developer options.

**Decision.** A default uncaught-exception handler, installed first in `attachBaseContext`,
writes a report to the app's own storage and shows it on a plain-view `Activity` running in a
`:crash` process, with a Share button. The handler then delegates to the platform's handler,
which ends the process as the system expects.

**Why a second process.** The crash this is most needed for is the one during application
startup, where the main process cannot reach a screen at all. A reporter hosted there would be
dead before it could draw.

**Why the report is also shown on the next launch.** Starting the crash screen from the handler
is a race: the request reaches the system while the process that made it is being torn down,
and the screen can be lost -- which is how the first crash this was built to diagnose produced
no screen at all. The report on disk does not race, so an unread one is shown the next time the
app is opened. That path is the reliable one; the screen from the handler is the fast one.

**Why platform views and hard-coded colours.** It has to render when the app's own theme,
resources or dependency graph are what failed, so it depends on none of them.

**What it costs.** A second process entry in the manifest, and a screen that is deliberately
uglier than the rest of the app. The handler also sees exceptions it has no business showing a
screen for — a crash on a worker thread during a background reconstruction now puts a crash
screen in front of a user who was doing something else. That is the right trade for a scanner
whose failures are otherwise invisible, but it is a trade.

**Why not a debug-only build flavour.** The crash that matters is the one on a user's device,
on their phone's OS version, with their data. A reporter that only exists in builds made by
people who already have logcat reports nothing.

---

## ADR-017 — The one-session rule is enforced in code, not by convention

**Status:** accepted

**Context.** ARCore permits one session per process and enforces it in native code. Breaking the
rule does not throw: the process dies, with no Kotlin exception for any handler to catch and
nothing in the app able to report it. Two bugs reached a device that way, and a third was
waiting. A renderer remembered on the capture mode built a second session on every mode switch.
A session whose close was queued for a GL thread that had already stopped was never closed at
all, so the second scan of a session met a session ARCore still considered open. And AR
placement constructs a session of its own, which would have met whatever capture left behind.

Each of those is a different call site making the same mistake, and each was written by someone
who knew the rule.

**Decision.** `ArSessionSlot` in `core:ar` holds the claim. A session claims it before touching
ARCore and releases it on close, including when closing threw. A second claim fails and the
screen says the camera is still busy.

**Why there.** Above both claimants, because capture is not the only one. In `feature:capture` it
would have protected capture from itself and left AR placement to find out the hard way.

**What it costs.** Process-wide mutable state, which the rest of the app does not have and which
makes two instances untestable in parallel in one JVM. The thing it guards is process-wide, so
there is nowhere smaller to put it, but it is still the kind of state that spreads if nobody
says why it is here. It also converts a class of bug from "dies in native code" to "shows a
message that is not quite right" -- a user whose previous scan leaked the slot is told the
camera is busy, which is true but does not name the bug. That is a trade worth making once and
not twice.

---

## ADR-018 — Renderer callbacks report instead of propagating

**Status:** accepted

**Context.** Anything thrown from a `GLSurfaceView.Renderer` callback propagates out of the GL
thread and ends the process. Filament's frame callback is on the main thread, which the crash
reporter can at least see, but the result is still the app closing. Both sit behind GPU drivers,
a camera service and native libraries that throw things no signature declares, on devices
nobody here has.

**Decision.** The capture renderer's three callbacks and the viewer's frame callback each catch
`Throwable`, stop the loop, and report a failure the screen shows. Capture offers Import, which
needs no AR; the viewer says the 3D view is unavailable and leaves export working.

**Why catch `Throwable`.** The point is the unanticipated case. An `Error` from a native library
is exactly as fatal to the user as an exception and exactly as reportable.

**What it costs.** A caught `Throwable` hides real programming errors in the renderer: a null
dereference in our own frame code now shows "scanning stopped unexpectedly" rather than
crashing loudly in a way that would get fixed. Both are logged at error level, and the renderer
stops rather than carrying on in an unknown state, but a bug in the drawing path is now quieter
than it should be. The alternative is an app that vanishes, which is worse for the person
holding it and no better for the person fixing it.

---

## ADR-019 — The depth sweep runs on several cores, capped at six

**Status:** accepted

**Context.** The engine was single-threaded throughout. That went unnoticed until the first real
photographs went through it: depth estimation was 97% of the run, forty seconds a frame on
1,600 px frames at the coarsest preset, on a four-core machine with three cores idle. The
pipeline is a background job on a phone that the user is not watching, but a scan that takes
twenty minutes is one they stop trusting.

**Decision.** A `parallelFor` over row blocks, used by the two pixel loops in the plane sweep.
Nothing else in the engine is threaded. The thread count asks the hardware and is capped at six.

**Why only the sweep.** It is where the time is, and it is the one place with no shared state:
each pixel's depth depends on the images and on nothing another pixel computed. Fusion,
meshing and texturing are each under three seconds on the same run, so threading them would add
races to guard against for no measured gain.

**Why row blocks rather than a work queue.** The cost per pixel is near-uniform, so a queue's
synchronisation would be a large fraction of the saving.

**What it measures.** 670 s to 138 s on sixteen real photographs, with the voxel, triangle and
point counts bit-identical. A test asserts the one-thread and four-thread depth maps are equal,
because the failure mode of a parallelisation is not slowness but a map that differs slightly
between runs, which is far harder to notice than a crash.

**What it costs.** Battery and heat, on a device that is already thermally limited while
scanning -- which is why the cap is six rather than the core count, and why it is a number
rather than "all of them". It is the wrong lever, though: the right fix is coarse-to-fine, which
would cut the work rather than spread it, and that remains undone. Threading made a bad
algorithm bearable, and an ADR that claimed otherwise would be hiding the real cost. There is
also no per-device thread budget yet: the cap is the same on a phone that is hot and one that is
cold, where the capture stage already throttles itself on thermal state and this does not.

