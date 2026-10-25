# Working in this repository

Conventions, and the reasoning behind them. Read [README.md](README.md) for what
the app is and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how it is put
together.

---

## The rule everything else follows from

**There is no GPU, no camera and no ARCore in CI.** A design that spreads
camera-dependent logic across several files is a design where most of the app
cannot be verified. So:

> Anything that needs hardware goes in exactly one file, and that file does as
> little as possible. Everything else is ordinary code with tests.

`ArCaptureSession`, `ArPlacementSession`, `ModelSurface`, `VulkanSplatTrainer`,
`MonocularDepthEstimator`, `FrameRecorder` — those are the files that cannot be
tested here, and that is the whole list. If you find yourself putting a
decision inside one of them, it belongs outside instead.

The second half of the rule: **when one of those paths fails, degrade rather
than break.** No Vulkan means the CPU splat trainer on a shorter schedule. No
LiteRT model means multi-view stereo. A renderer that will not start means the
viewer says so and export still works. A device without ARCore keeps import,
processing, the viewer and every export.

---

## Before you commit

```bash
./gradlew build detekt     # assemble, unit tests, Lint, R8, detekt
./build-host/parallax_tests
```

Both must pass. Do not move on with a broken build — it is far cheaper to fix a
failure now than to find it three changes later underneath two other ones.

If the native engine changed:

```bash
cmake --build build-host --parallel && ./build-host/parallax_tests
```

If the geometry changed, run it on photographs as well as on the synthetic captures:

```bash
./build-host/parallax_run <folder-of-photographs> /tmp/work --mode room --preset fast
```

The synthetic captures are rendered by the same code that consumes them, in the same
conventions, with no lens distortion, no JPEG artefacts and no camera a person ever moved. They
passed through every failure the first real device reported. The first real dataset put through
`parallax_run` found four bugs in an afternoon -- unverified matches, a PnP degenerate on
planar scenes, features detected at the source resolution, and a frame excluded for failing
once. Add `--keep-manifest` to run a project directory built elsewhere, which is how a dataset
with real camera poses exercises the path a device capture takes.

If a Compose screen or a design token changed, the screenshots will differ.
Look at the difference before re-recording it:

```bash
./gradlew verifyRoborazziDebug   # see what moved
./gradlew recordRoborazziDebug   # accept it
```

---

## Tests

**A test's name is a sentence about behaviour, not a label for a method.**
`` `a drag stops any fling that was still running` ``, not `testFling`. If the
name cannot be written as a sentence, the test is probably checking an
implementation detail.

**Test the thing the code is for.** The camera is checked by requiring that
framing a box puts every corner inside the frustum, not by asserting a
particular distance. The splat trainer is checked by holding a camera out of
the training set and requiring the error on that unseen view to halve — a claim
about geometry, not about memorising the training images.

**Prefer the real collaborator to a fake.** ViewModel tests drive real
repositories against an in-memory database with direct executors. A fake
repository only ever asserts that the ViewModel calls the methods the ViewModel
calls.

**Gradient checks for anything differentiable.** Every analytic gradient in the
splat code is checked against central differences, scored with a smooth
objective so the check tests the gradient rather than the loss. Three real bugs
came out of that, each of which would have trained to a worse model rather than
crashing.

**When a test fails, work out which side is wrong.** Several times here the
test was right and the code was wrong; several times the reverse. Changing the
assertion to match the output is only correct when you can say *why* the new
behaviour is better, in the comment you leave behind.

---

## Comments

Comments say **why**, never what. If a comment restates the line below it,
delete it.

```kotlin
// Good: this is the thing you cannot recover from the code.
// Pitch is clamped just short of the poles rather than at them: exactly
// overhead the azimuth is undefined and the up vector degenerates, which
// shows up as the model spinning on the spot.
val pitch = (pitch + delta).coerceIn(-MAX_PITCH, MAX_PITCH)

// Bad: the code already says this.
// Clamp the pitch.
val pitch = (pitch + delta).coerceIn(-MAX_PITCH, MAX_PITCH)
```

A constant with a non-obvious value gets a comment explaining the value, not the
name. `kIdealBaselineRatio = 0.1` is meaningless without "views closer than this
have no parallax and views further have no overlap".

---

## Style

- **Kotlin** — explicit over clever. Named arguments where the call site would
  otherwise be a row of booleans. `data class` for state; sealed interfaces for
  things with a fixed set of shapes.
- **C++17** — no exceptions across the JNI boundary, no RTTI, `Result`-shaped
  returns (`bool` plus an out-parameter and an error string). Eigen is the only
  mandatory dependency.
- **Compose** — screens take state and callbacks; ViewModels own all state,
  including half-finished work like a measurement three taps in. A rotation
  must not throw that away.
- **Naming** — say what a thing is for. `estimateMissingDepth`, not
  `processDepth2`.
- **Line length** 100 for Kotlin, 100 for C++. detekt enforces 120 as a
  backstop; aim for 100.

---

## Errors

Every failure a user can see is a `ParallaxError` with a message that is free of
jargon, never blames them, and carries the single most useful next action. An
error with no recovery is a dead end; that is why `RecoveryAction` exists and
why `ErrorState` takes the error rather than loose strings.

```kotlin
// This, with a recovery the screen can offer:
ParallaxError.InsufficientStorage(requiredBytes, availableBytes)
// Not this:
throw IllegalStateException("ENOSPC")
```

---

## Adding a dependency

Three things, in the same change:

1. Pin the version in `gradle/libs.versions.toml`. Check the current version
   against the live metadata — not from memory, which goes stale.
2. Add it to [docs/LICENSES.md](docs/LICENSES.md) with its licence.
3. Check the licence is MIT, BSD, Apache-2.0 or MPL-2.0. **No GPL or AGPL.**
   This is why OpenMVS was passed over for multi-view stereo and why only the
   Small Depth Anything weights are usable.

The same applies to model weights, which have licences of their own and are
easy to forget.

---

## Decisions

When a choice is non-obvious or you rejected a reasonable alternative, add an
ADR to [docs/DECISIONS.md](docs/DECISIONS.md): the context, the decision, why,
and — the part that matters most later — **what it costs**. An ADR that only
lists advantages is not finished.

ADR-009 was rewritten rather than replaced when the original "no CPU fallback"
decision turned out to be based on an estimate made before the reference
existed. That is the right thing to do: supersede it in place and say why.

---

## Keeping `PROGRESS.md` honest

At the end of a phase: build, run everything, update `PROGRESS.md`, commit.

`PROGRESS.md` carries a per-phase table of **the bugs that phase's tests
caught**, with the symptom and the cause. Not the tests written, not the
features added — the things that were wrong and would have shipped. That table
is the most useful part of the file, because it is the evidence that the tests
are worth their cost.

It also carries a "verification boundaries" section listing what genuinely
cannot be checked here. Keep it accurate. A claim that something is tested when
it is not is worse than no claim at all.

---

## Things that have already gone wrong here

Worth knowing, because they are easy to repeat.

- **Detekt's `excludes` replaces its default**, which exempts test sources. Add
  one glob and you have silently enabled `MagicNumber` across every test.
- **A `[&]` lambda over a block-scoped C++ fixture** is a dangling reference by
  the time a deferred call runs. That is a segfault in the benchmark harness.
- **Robolectric and Compose disagree above API 35.** Screenshot tests pin
  `sdk = [35]`; the newest image lacks `InputManager.getInstance()`, which
  Compose's test rule reflects on.
- **The kotlinx-serialization *plugin* is separate from the dependency.** A
  module with the dependency but not the plugin compiles cleanly and throws
  `SerializationException` at runtime.
- **Expedited WorkManager jobs only accept network and storage constraints.**
  Battery and charging constraints make `enqueue` throw.
- **A theme that does not paint its own background** inherits the activity's
  window background, which follows the *system* dark mode rather than the app's
  setting.
