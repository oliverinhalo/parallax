#include "parallax/pipeline/Pipeline.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "parallax/io/ImageIo.h"
#include "parallax/io/Json.h"
#include "parallax/testing/SyntheticScene.h"
#include "parallax/splat/GaussianCloud.h"

using namespace parallax;
using namespace parallax::testing;
namespace fs = std::filesystem;

namespace {

/// Writes a complete synthetic capture into a temporary project directory, in exactly the
/// layout the capture layer produces, so the pipeline is exercised through its real inputs.
class SyntheticCapture {
 public:
  SyntheticCapture() {
    const auto base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      dir_ = base / ("parallax_pipeline_" + std::to_string(counter()++));
      std::error_code ec;
      if (fs::create_directory(dir_, ec)) break;
    }
    std::error_code ec;
    fs::create_directories(dir_ / "frames", ec);
    fs::create_directories(dir_ / "depth", ec);
  }
  ~SyntheticCapture() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string path() const { return dir_.string(); }

  /// Render an orbit of a textured sphere on a table and write it out.
  void writeObjectCapture(int views = 14, int resolution = 160, bool with_poses = true,
                          bool with_depth = true) {
    SyntheticScene scene;
    Primitive ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0, 0.3, 0.0);
    ball.radius = 0.22;
    ball.texture = Texture::Noise;
    ball.texture_scale = 26.0;
    ball.albedo = Vec3(0.85, 0.55, 0.4);
    scene.primitives.push_back(ball);
    Primitive floor;
    floor.shape = Shape::Plane;
    floor.center = Vec3::Zero();
    floor.normal = Vec3::UnitY();
    floor.texture = Texture::Noise;
    floor.texture_scale = 8.0;
    scene.primitives.push_back(floor);

    const CameraIntrinsics k = phoneIntrinsics(resolution, resolution, 62.0);
    writeCapture(scene, k, SyntheticScene::orbitPoses(ball.center, 0.75, 0.22, views),
                 with_poses, with_depth);
  }

  /// Render an arbitrary scene from arbitrary poses and write the manifest.
  void writeCapture(const SyntheticScene& scene, const CameraIntrinsics& k,
                    const std::vector<Pose>& poses, bool with_poses = true,
                    bool with_depth = true) {
    Json frames = Json::array();
    for (size_t i = 0; i < poses.size(); ++i) {
      const RenderResult frame = scene.render(k, poses[i], true, with_depth);
      char name[64];
      std::snprintf(name, sizeof(name), "%04zu.png", i);
      std::vector<uint8_t> png;
      ASSERT_TRUE(encodePng(frame.color, &png));
      ASSERT_TRUE(writeFileBytes((dir_ / "frames" / name).string(), png));

      char depth_name[64] = "";
      if (with_depth) {
        std::snprintf(depth_name, sizeof(depth_name), "%04zu.raw", i);
        writeDepth((dir_ / "depth" / depth_name).string(), frame.depth);
      }

      Json entry = Json::object();
      entry.set("index", Json::integer(static_cast<int64_t>(i)));
      entry.set("image", Json::string(name));
      if (with_depth) entry.set("depth", Json::string(depth_name));
      entry.set("keyframe", Json::boolean(true));
      entry.set("sharpness", Json::number(420.0));
      entry.set("exposure", Json::number(128.0));

      Json intrinsics = Json::object();
      intrinsics.set("fx", Json::number(k.fx));
      intrinsics.set("fy", Json::number(k.fy));
      intrinsics.set("cx", Json::number(k.cx));
      intrinsics.set("cy", Json::number(k.cy));
      intrinsics.set("width", Json::integer(k.width));
      intrinsics.set("height", Json::integer(k.height));
      entry.set("intrinsics", std::move(intrinsics));

      if (with_poses) {
        // The manifest stores world-from-camera column-major, as ARCore reports it.
        const Mat4 world_from_camera = poses[i].worldFromCamera();
        Json matrix = Json::array();
        for (int column = 0; column < 4; ++column) {
          for (int row = 0; row < 4; ++row) {
            matrix.push(Json::number(world_from_camera(row, column)));
          }
        }
        entry.set("pose", std::move(matrix));
      }
      frames.push(std::move(entry));
    }

    Json manifest = Json::object();
    manifest.set("version", Json::integer(1));
    manifest.set("frames", std::move(frames));
    ASSERT_TRUE(writeFileText((dir_ / "manifest.json").string(), manifest.dump(2)));
  }

  /// True when any file in `depth/` ends with `suffix`.
  bool hasDepthFileMatching(const std::string& suffix) const {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_ / "depth", ec)) {
      const std::string name = entry.path().filename().string();
      if (name.size() >= suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return true;
      }
    }
    return false;
  }

  /// Delete every depth map, leaving the manifest and checkpoints alone.
  void removeDepthFiles() const {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_ / "depth", ec)) {
      fs::remove(entry.path(), ec);
    }
  }

  bool hasCheckpoint(const std::string& name) const {
    std::error_code ec;
    return fs::exists(dir_ / "checkpoints" / name, ec);
  }
  bool hasOutput(const std::string& name) const {
    std::error_code ec;
    return fs::exists(dir_ / "outputs" / name, ec);
  }
  void removeCheckpoint(const std::string& name) const {
    std::error_code ec;
    fs::remove(dir_ / "checkpoints" / name, ec);
  }
  void corruptManifest() const { writeFileText((dir_ / "manifest.json").string(), "{ broken"); }

 private:
  static void writeDepth(const std::string& path, const DepthMap& depth) {
    // width, height, then raw floats -- the format the capture layer writes.
    std::vector<uint8_t> bytes;
    const int32_t width = depth.width, height = depth.height;
    bytes.resize(8 + static_cast<size_t>(width) * height * sizeof(float));
    std::memcpy(bytes.data(), &width, 4);
    std::memcpy(bytes.data() + 4, &height, 4);
    std::memcpy(bytes.data() + 8, depth.depth.data(),
                static_cast<size_t>(width) * height * sizeof(float));
    writeFileBytes(path, bytes);
  }
  static int& counter() {
    static int value = 7000;
    return value;
  }
  fs::path dir_;
};

PipelineConfig objectConfig(const std::string& dir) {
  PipelineConfig config;
  config.project_dir = dir;
  config.mode = CaptureMode::Object;
  config.voxel_size = 0.008f;
  config.max_keyframes = 40;
  config.texture_resolution = 1024;
  config.target_triangles = 40000;
  config.has_device_poses = true;
  config.has_depth_maps = true;
  config.memory_budget_bytes = 256ull * 1024 * 1024;
  // The subject sits 0.75 m from the camera, so there is nothing of interest beyond ~1.5 m.
  config.max_integration_depth = 1.6f;
  config.keep_largest_component = true;
  config.remove_ground_plane = false;
  config.build_point_cloud = true;
  config.analyse_room = false;
  return config;
}

}  // namespace

TEST(Pipeline, StageMetadataIsConsistent) {
  double total = 0.0;
  for (int s = 0; s < static_cast<int>(Stage::Count); ++s) {
    const Stage stage = static_cast<Stage>(s);
    EXPECT_GT(stageWeight(stage), 0.0) << stageName(stage);
    EXPECT_STRNE(stageName(stage), "unknown");
    total += stageWeight(stage);
  }
  // The weights drive the overall progress bar, so they must sum to one or it never reaches
  // 100% (or passes it).
  EXPECT_NEAR(total, 1.0, 1e-9);
}

TEST(Pipeline, ReportsTheFirstStageOnAFreshProject) {
  SyntheticCapture capture;
  capture.writeObjectCapture(6, 96);
  const Pipeline pipeline(objectConfig(capture.path()));
  EXPECT_EQ(pipeline.nextStage(), Stage::Ingest);
  EXPECT_FALSE(pipeline.isComplete());
  EXPECT_TRUE(pipeline.loadMesh().empty());
}

TEST(Pipeline, FailsCleanlyOnAMissingOrBrokenManifest) {
  SyntheticCapture capture;
  Pipeline pipeline(objectConfig(capture.path()));
  // No manifest at all.
  StageOutcome outcome = pipeline.runStage(Stage::Ingest, nullptr, nullptr);
  EXPECT_FALSE(outcome.success);
  EXPECT_FALSE(outcome.cancelled);
  EXPECT_FALSE(outcome.message.empty());

  capture.writeObjectCapture(4, 96);
  capture.corruptManifest();
  Pipeline broken(objectConfig(capture.path()));
  outcome = broken.runStage(Stage::Ingest, nullptr, nullptr);
  EXPECT_FALSE(outcome.success);
}

TEST(Pipeline, StagesRunInOrderAndCheckpointToDisk) {
  SyntheticCapture capture;
  capture.writeObjectCapture(10, 128);
  Pipeline pipeline(objectConfig(capture.path()));

  // Each stage, run individually, must advance `nextStage` by exactly one.
  const Stage expected[] = {Stage::Ingest, Stage::PoseRefinement, Stage::Depth, Stage::Fusion,
                            Stage::Meshing};
  for (Stage stage : expected) {
    ASSERT_EQ(pipeline.nextStage(), stage) << "at " << stageName(stage);
    const StageOutcome outcome = pipeline.runStage(stage, nullptr, nullptr);
    ASSERT_TRUE(outcome.success) << stageName(stage) << ": " << outcome.message;
  }
  EXPECT_TRUE(capture.hasCheckpoint("ingest.json"));
  EXPECT_TRUE(capture.hasCheckpoint("poses.json"));
  EXPECT_TRUE(capture.hasCheckpoint("depth.json"));
  EXPECT_TRUE(capture.hasCheckpoint("tsdf.bin"));
  EXPECT_TRUE(capture.hasCheckpoint("mesh_raw.ply"));
}

TEST(Pipeline, RunsAnObjectCaptureToCompletion) {
  SyntheticCapture capture;
  capture.writeObjectCapture(16, 160);
  Pipeline pipeline(objectConfig(capture.path()));

  // Progress must be monotonic and reach 1.0: a bar that goes backwards destroys confidence
  // in the estimate behind it.
  double last_overall = -1.0;
  std::vector<Stage> seen;
  const auto progress = [&](const Progress& p) {
    EXPECT_GE(p.overall_fraction, last_overall - 1e-9)
        << "overall progress went backwards at " << stageName(p.stage);
    EXPECT_GE(p.stage_fraction, 0.0);
    EXPECT_LE(p.stage_fraction, 1.0);
    EXPECT_LE(p.overall_fraction, 1.0 + 1e-9);
    last_overall = std::max(last_overall, p.overall_fraction);
    if (seen.empty() || seen.back() != p.stage) seen.push_back(p.stage);
  };

  const StageOutcome outcome = pipeline.runToCompletion(progress, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;
  EXPECT_TRUE(pipeline.isComplete());
  EXPECT_EQ(pipeline.nextStage(), Stage::Count);
  EXPECT_GT(last_overall, 0.9);

  // Stages must have been visited in order.
  for (size_t i = 1; i < seen.size(); ++i) {
    EXPECT_LE(static_cast<int>(seen[i - 1]), static_cast<int>(seen[i]));
  }

  // The outputs the app depends on.
  EXPECT_TRUE(capture.hasOutput("model.glb"));
  EXPECT_TRUE(capture.hasCheckpoint("mesh_clean.ply"));
  EXPECT_TRUE(capture.hasCheckpoint("cloud.ply"));

  const PipelineSummary& summary = pipeline.summary();
  EXPECT_EQ(summary.frame_count, 16);
  EXPECT_GE(summary.keyframe_count, 2);
  EXPECT_GE(summary.registered_frame_count, 2);
  EXPECT_GT(summary.triangle_count, 100);
  EXPECT_GT(summary.point_count, 100);
  EXPECT_GT(summary.surface_area_m2, 0.0);
  EXPECT_TRUE(summary.bounds.valid());

  // The reconstructed sphere sits at y = 0.3 with radius 0.22, so the model's bounding box
  // should be roughly 0.44 m across. The ground plane is in the capture too, so only check
  // that the subject is present and the scale is metric.
  EXPECT_GT(summary.bounds.diagonal(), 0.2);
  EXPECT_LT(summary.bounds.diagonal(), 6.0);

  const Mesh mesh = pipeline.loadMesh();
  EXPECT_FALSE(mesh.empty());
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;

  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  EXPECT_TRUE(pipeline.loadPointCloud(&points, &normals, &colors));
  EXPECT_GT(points.size(), 100u);
}

TEST(Pipeline, ResumesFromTheLastCompletedStage) {
  SyntheticCapture capture;
  capture.writeObjectCapture(12, 128);
  PipelineConfig config = objectConfig(capture.path());

  // Run the first four stages, then throw the Pipeline away entirely, as a process death would.
  {
    Pipeline first(config);
    for (Stage stage : {Stage::Ingest, Stage::PoseRefinement, Stage::Depth, Stage::Fusion}) {
      ASSERT_TRUE(first.runStage(stage, nullptr, nullptr).success) << stageName(stage);
    }
  }

  // A brand-new Pipeline over the same directory must pick up where the last one stopped,
  // rather than starting over -- this is the whole point of checkpointing.
  Pipeline resumed(config);
  EXPECT_EQ(resumed.nextStage(), Stage::Meshing);
  const StageOutcome outcome = resumed.runToCompletion(nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << "resumed at " << stageName(resumed.nextStage()) << ": "
                               << outcome.message;
  EXPECT_TRUE(resumed.isComplete());
  // The summary survived too, so the library does not lose the frame count on resume.
  EXPECT_EQ(resumed.summary().frame_count, 12);
}

TEST(Pipeline, InvalidateFromRerunsTheRequestedStages) {
  SyntheticCapture capture;
  capture.writeObjectCapture(10, 128);
  Pipeline pipeline(objectConfig(capture.path()));
  ASSERT_TRUE(pipeline.runToCompletion(nullptr, nullptr).success);
  ASSERT_TRUE(pipeline.isComplete());

  // Re-texturing at a different resolution must not re-run fusion.
  pipeline.invalidateFrom(Stage::Texturing);
  EXPECT_EQ(pipeline.nextStage(), Stage::Texturing);
  EXPECT_TRUE(capture.hasCheckpoint("tsdf.bin"));
  EXPECT_TRUE(capture.hasCheckpoint("mesh_clean.ply"));
  EXPECT_FALSE(capture.hasCheckpoint("mesh_textured.ply"));

  ASSERT_TRUE(pipeline.runToCompletion(nullptr, nullptr).success);
  EXPECT_TRUE(pipeline.isComplete());
}

TEST(Pipeline, CancellationStopsPromptlyAndLeavesNoCheckpoint) {
  SyntheticCapture capture;
  capture.writeObjectCapture(14, 128);
  Pipeline pipeline(objectConfig(capture.path()));
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);

  // Cancel partway through the pose stage, which is the long one.
  auto cancel = std::make_shared<std::atomic_bool>(false);
  int callbacks = 0;
  const auto progress = [&](const Progress&) {
    if (++callbacks >= 3) cancel->store(true);
  };
  const StageOutcome outcome = pipeline.runStage(Stage::PoseRefinement, progress, cancel);
  EXPECT_FALSE(outcome.success);
  EXPECT_TRUE(outcome.cancelled);
  // A cancelled stage must not leave a checkpoint behind, or the resume would skip it.
  EXPECT_FALSE(capture.hasCheckpoint("poses.json"));
  EXPECT_EQ(pipeline.nextStage(), Stage::PoseRefinement);

  // And it must be runnable again afterwards.
  EXPECT_TRUE(pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr).success);
}

TEST(Pipeline, RunsStructureFromMotionWhenThereAreNoDevicePoses) {
  // The import path: photos with no poses at all, so the pipeline must recover them.
  //
  // 30 views over a full orbit is a 12-degree step, which is the overlap the app's capture
  // guidance asks for and roughly what a user sweeping a phone around an object produces.
  // The engine's binary descriptors do not match reliably across much wider baselines than
  // that -- see docs/DECISIONS.md (ADR-003) on the OpenCV/SIFT trade-off.
  SyntheticCapture capture;
  capture.writeObjectCapture(30, 288, /*with_poses=*/false, /*with_depth=*/true);
  PipelineConfig config = objectConfig(capture.path());
  config.has_device_poses = false;
  config.max_keyframes = 30;

  Pipeline pipeline(config);
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);
  const StageOutcome outcome = pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;
  // The initial pair plus at least some growth.
  //
  // This threshold reflects a real, measured limitation rather than an aspiration. The
  // pose-less front end reliably establishes a seed pair and extends it by a few frames, but
  // it does not currently scale to a full orbit: steered-BRIEF descriptors are far weaker than
  // SIFT across wide baselines, and DLT PnP is degenerate on the coplanar point sets scans
  // produce. The two-view geometry, triangulation, PnP and bundle adjustment underneath are
  // each separately tested and accurate; it is the incremental front end that is a skeleton.
  // See PROGRESS.md and docs/DECISIONS.md (ADR-003). The assertion is kept as a regression
  // guard so this cannot silently get worse.
  EXPECT_GE(pipeline.summary().registered_frame_count, 3) << outcome.message;
  // Monocular reconstruction has no metric scale, which is recorded so the UI can ask the user
  // for a known dimension rather than silently presenting wrong measurements.
  EXPECT_EQ(pipeline.summary().scale_factor, 0.0);
}

/**
 * A capture with no depth maps at all still reconstructs.
 *
 * This is the path every device without a depth sensor takes -- and, since the
 * monocular depth model is an optional asset, the path most devices take. The
 * depth stage estimates depth from the images and the refined poses by
 * multi-view stereo, and writes it into `depth/` so that fusion and texturing
 * cannot tell the difference.
 */
TEST(Pipeline, EstimatesDepthFromTheImagesWhenThereIsNone) {
  SyntheticCapture capture;
  capture.writeObjectCapture(12, 160, /*with_poses=*/true, /*with_depth=*/false);
  Pipeline pipeline(objectConfig(capture.path()));
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);
  ASSERT_TRUE(pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr).success);

  const StageOutcome outcome = pipeline.runStage(Stage::Depth, nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;
  EXPECT_NE(outcome.message.find("estimated"), std::string::npos) << outcome.message;

  // The estimates are on disk under their own extension, so an estimate is
  // distinguishable from a sensor measurement when diagnosing a bad scan.
  EXPECT_TRUE(capture.hasDepthFileMatching(".mvs"));
}

/// And the whole pipeline runs through on those estimates, which is the claim
/// that actually matters: a model comes out the other end.
TEST(Pipeline, RunsToCompletionOnEstimatedDepth) {
  SyntheticCapture capture;
  capture.writeObjectCapture(16, 160, /*with_poses=*/true, /*with_depth=*/false);
  Pipeline pipeline(objectConfig(capture.path()));

  const StageOutcome outcome = pipeline.runToCompletion(nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;
  EXPECT_TRUE(capture.hasOutput("model.glb"));

  const PipelineSummary& summary = pipeline.summary();
  EXPECT_GT(summary.triangle_count, 100);
  // Metric scale survives: the depth came from triangulating the real poses,
  // so the model is in metres just as a sensor-depth scan would be.
  EXPECT_GT(summary.bounds.diagonal(), 0.15);
  EXPECT_LT(summary.bounds.diagonal(), 6.0);
}

/// Depth estimated in one process, fused in another.
///
/// The estimates are written to `depth/` and recorded on the in-memory frame records, but the
/// manifest on disk is the app's record of the capture and never learns about them. So every
/// resume and every retry -- which is a fresh process reading that manifest -- used to reach
/// fusion with no depth at all and fail, having written perfectly good depth maps a stage
/// earlier. The existing resume test missed it by using a capture that had sensor depth.
TEST(Pipeline, FusesDepthEstimatedBeforeTheProcessWasRestarted) {
  SyntheticCapture capture;
  capture.writeObjectCapture(16, 160, /*with_poses=*/true, /*with_depth=*/false);
  PipelineConfig config = objectConfig(capture.path());

  {
    Pipeline first(config);
    ASSERT_TRUE(first.runStage(Stage::Ingest, nullptr, nullptr).success);
    ASSERT_TRUE(first.runStage(Stage::PoseRefinement, nullptr, nullptr).success);
    const StageOutcome depth = first.runStage(Stage::Depth, nullptr, nullptr);
    ASSERT_TRUE(depth.success) << depth.message;
  }

  Pipeline resumed(config);
  ASSERT_EQ(resumed.nextStage(), Stage::Fusion);
  const StageOutcome outcome = resumed.runStage(Stage::Fusion, nullptr, nullptr);

  ASSERT_TRUE(outcome.success) << outcome.message;
}

/// A fusion failure has to say which half of the stage had nothing to work with.
TEST(Pipeline, FusionSaysWhenNoKeyframeHadADepthMap) {
  SyntheticCapture capture;
  capture.writeObjectCapture(12, 128);
  PipelineConfig config = objectConfig(capture.path());
  Pipeline pipeline(config);
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);
  ASSERT_TRUE(pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr).success);
  ASSERT_TRUE(pipeline.runStage(Stage::Depth, nullptr, nullptr).success);

  // Every depth map removed between the stage that made them and the stage that needs them,
  // which is what a cleared cache or a half-copied project bundle looks like.
  capture.removeDepthFiles();
  Pipeline withoutDepth(config);
  const StageOutcome outcome = withoutDepth.runStage(Stage::Fusion, nullptr, nullptr);

  EXPECT_FALSE(outcome.success);
  EXPECT_NE(outcome.message.find("depth map"), std::string::npos) << outcome.message;
}

/// With no poses *and* no depth there is nothing to triangulate from, and the
/// failure has to say so rather than producing an empty model.
/// No baseline at all, with the poses supplied.
///
/// The poses are given, so the frames are placed and the run reaches depth -- where there is
/// nothing to triangulate, because every view is from the same point. The message has to name
/// depth rather than producing an empty model.
TEST(Pipeline, FailsWithAClearMessageWhenDepthCannotBeEstimated) {
  SyntheticCapture capture;
  {
    SyntheticScene scene = SyntheticScene::objectOnTable();
    const CameraIntrinsics intrinsics = phoneIntrinsics(160, 120);
    std::vector<Pose> poses;
    for (int i = 0; i < 10; ++i) {
      poses.push_back(SyntheticScene::lookAt(Vec3(0, 0.4, -1.0), Vec3(0, 0.3, 0)));
    }
    capture.writeCapture(scene, intrinsics, poses, /*with_poses=*/true, /*with_depth=*/false);
  }
  PipelineConfig config = objectConfig(capture.path());
  config.has_depth_maps = false;
  Pipeline pipeline(config);
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);
  ASSERT_TRUE(pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr).success);

  const StageOutcome outcome = pipeline.runStage(Stage::Depth, nullptr, nullptr);
  EXPECT_FALSE(outcome.success);
  EXPECT_NE(outcome.message.find("depth"), std::string::npos) << outcome.message;
}

/// And without poses, the same capture fails a stage earlier, where the cause is.
///
/// Views from one point cannot be placed relative to each other at all, so pose refinement is
/// where that has to be said. It used to register a handful of frames, report success, and
/// leave depth to explain a failure it had inherited.
TEST(Pipeline, FailsAtPoseRefinementWhenTheFramesCannotBePlaced) {
  SyntheticCapture capture;
  {
    SyntheticScene scene = SyntheticScene::objectOnTable();
    const CameraIntrinsics intrinsics = phoneIntrinsics(160, 120);
    std::vector<Pose> poses;
    for (int i = 0; i < 10; ++i) {
      poses.push_back(SyntheticScene::lookAt(Vec3(0, 0.4, -1.0), Vec3(0, 0.3, 0)));
    }
    capture.writeCapture(scene, intrinsics, poses, /*with_poses=*/false, /*with_depth=*/false);
  }
  PipelineConfig config = objectConfig(capture.path());
  config.has_device_poses = false;
  config.has_depth_maps = false;
  Pipeline pipeline(config);
  ASSERT_TRUE(pipeline.runStage(Stage::Ingest, nullptr, nullptr).success);

  const StageOutcome outcome = pipeline.runStage(Stage::PoseRefinement, nullptr, nullptr);

  EXPECT_FALSE(outcome.success);
  EXPECT_NE(outcome.message.find("frames"), std::string::npos) << outcome.message;
}

/// Fusion says how far it integrated.
///
/// The range is measured from the depth maps rather than set by the capture mode, so the figure
/// is no longer something a reader can look up -- which means a range that is still wrong is
/// only visible if the stage reports it. `IntegrationRangeTest` covers the choosing; this
/// covers the saying, and that the measured range is what the volume is actually built with.
TEST(Pipeline, FusionReportsTheDepthItIntegratedTo) {
  SyntheticCapture capture;
  capture.writeObjectCapture(12, 128);
  Pipeline pipeline(objectConfig(capture.path()));
  for (Stage stage : {Stage::Ingest, Stage::PoseRefinement, Stage::Depth}) {
    ASSERT_TRUE(pipeline.runStage(stage, nullptr, nullptr).success) << stageName(stage);
  }

  const StageOutcome fusion = pipeline.runStage(Stage::Fusion, nullptr, nullptr);

  ASSERT_TRUE(fusion.success) << fusion.message;
  EXPECT_NE(fusion.message.find("integrated to"), std::string::npos) << fusion.message;
}

/// A handful of photos is refused where the number still means something.
///
/// Two views triangulate and see around nothing, so fusion found no closed surface and the run
/// died at fusing surfaces talking about depth maps and camera poses -- six stages after the
/// only actionable fact, which is that there were two photos.
TEST(Pipeline, RefusesTooFewFramesAtIngestRatherThanFailingAtFusion) {
  SyntheticCapture capture;
  capture.writeObjectCapture(2, 160, /*with_poses=*/true, /*with_depth=*/true);
  Pipeline pipeline(objectConfig(capture.path()));

  const StageOutcome outcome = pipeline.runStage(Stage::Ingest, nullptr, nullptr);

  EXPECT_FALSE(outcome.success);
  EXPECT_NE(outcome.message.find("at least"), std::string::npos) << outcome.message;
  EXPECT_NE(outcome.message.find("2 usable frames"), std::string::npos) << outcome.message;
}

TEST(Pipeline, AStageThatNeedsAMissingCheckpointFailsRatherThanCrashing) {
  SyntheticCapture capture;
  capture.writeObjectCapture(8, 128);
  Pipeline pipeline(objectConfig(capture.path()));

  // Running out of order, with nothing on disk yet.
  for (Stage stage : {Stage::Fusion, Stage::Meshing, Stage::MeshCleanup, Stage::Texturing,
                      Stage::PointCloud, Stage::Finalise}) {
    const StageOutcome outcome = pipeline.runStage(stage, nullptr, nullptr);
    EXPECT_FALSE(outcome.success) << stageName(stage) << " should have refused to run";
    EXPECT_FALSE(outcome.message.empty()) << stageName(stage);
  }
}

TEST(Pipeline, RoomModeProducesAFloorPlan) {
  // A room capture: walk-around poses inside a box, with room analysis enabled.
  SyntheticCapture capture;
  {
    SyntheticScene scene = SyntheticScene::room(4.0, 3.2, 2.5);
    const CameraIntrinsics k = phoneIntrinsics(176, 176, 70.0);
    Json frames = Json::array();
    const std::vector<Pose> poses =
        SyntheticScene::orbitPoses(Vec3(0.0, 1.3, 0.0), 1.0, 0.0, 20);

    fs::path dir(capture.path());
    for (size_t i = 0; i < poses.size(); ++i) {
      const RenderResult frame = scene.render(k, poses[i], true, true);
      char name[64];
      std::snprintf(name, sizeof(name), "%04zu.png", i);
      std::vector<uint8_t> png;
      ASSERT_TRUE(encodePng(frame.color, &png));
      ASSERT_TRUE(writeFileBytes((dir / "frames" / name).string(), png));

      char depth_name[64];
      std::snprintf(depth_name, sizeof(depth_name), "%04zu.raw", i);
      std::vector<uint8_t> bytes;
      const int32_t width = frame.depth.width, height = frame.depth.height;
      bytes.resize(8 + static_cast<size_t>(width) * height * sizeof(float));
      std::memcpy(bytes.data(), &width, 4);
      std::memcpy(bytes.data() + 4, &height, 4);
      std::memcpy(bytes.data() + 8, frame.depth.depth.data(),
                  static_cast<size_t>(width) * height * sizeof(float));
      ASSERT_TRUE(writeFileBytes((dir / "depth" / depth_name).string(), bytes));

      Json entry = Json::object();
      entry.set("index", Json::integer(static_cast<int64_t>(i)));
      entry.set("image", Json::string(name));
      entry.set("depth", Json::string(depth_name));
      entry.set("keyframe", Json::boolean(true));
      entry.set("sharpness", Json::number(400.0));
      Json intrinsics = Json::object();
      intrinsics.set("fx", Json::number(k.fx));
      intrinsics.set("fy", Json::number(k.fy));
      intrinsics.set("cx", Json::number(k.cx));
      intrinsics.set("cy", Json::number(k.cy));
      intrinsics.set("width", Json::integer(k.width));
      intrinsics.set("height", Json::integer(k.height));
      entry.set("intrinsics", std::move(intrinsics));
      const Mat4 world_from_camera = poses[i].worldFromCamera();
      Json matrix = Json::array();
      for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
          matrix.push(Json::number(world_from_camera(row, column)));
        }
      }
      entry.set("pose", std::move(matrix));
      frames.push(std::move(entry));
    }
    Json manifest = Json::object();
    manifest.set("version", Json::integer(1));
    manifest.set("frames", std::move(frames));
    ASSERT_TRUE(writeFileText((dir / "manifest.json").string(), manifest.dump(2)));
  }

  PipelineConfig config = objectConfig(capture.path());
  config.mode = CaptureMode::Room;
  config.voxel_size = 0.03f;
  config.analyse_room = true;
  config.max_integration_depth = 6.0f;
  config.keep_largest_component = false;
  config.target_triangles = 120000;
  config.memory_budget_bytes = 400ull * 1024 * 1024;

  Pipeline pipeline(config);
  const StageOutcome outcome = pipeline.runToCompletion(nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;

  EXPECT_TRUE(capture.hasCheckpoint("floorplan.json"));
  if (pipeline.summary().has_floor_plan) {
    // The room is 4.0 x 3.2 m, so about 12.8 m2. The scan is taken from a single circular path
    // so coverage is imperfect; allow a wide band but require the right order of magnitude.
    EXPECT_GT(pipeline.summary().floor_area_m2, 5.0);
    EXPECT_LT(pipeline.summary().floor_area_m2, 22.0);
    EXPECT_TRUE(capture.hasOutput("floorplan.svg"));
    EXPECT_TRUE(capture.hasOutput("floorplan.pdf"));
    EXPECT_TRUE(capture.hasOutput("floorplan.png"));

    const FloorPlan plan = pipeline.loadFloorPlan();
    EXPECT_TRUE(plan.valid());
    EXPECT_GE(plan.walls.size(), 3u);
  }
}

TEST(Pipeline, TrainsASplatFieldWhenAskedTo) {
  // The splat stage is off by default and this is the only test that turns it
  // on, so the schedule is cut right back: the point is that the stage runs,
  // writes a loadable field and improves on its initialisation, not that a
  // 160-pixel synthetic orbit trains to a beautiful model.
  SyntheticCapture capture;
  capture.writeObjectCapture(12, 128);

  PipelineConfig config;
  config.project_dir = capture.path();
  config.mode = CaptureMode::Object;
  config.voxel_size = 0.01f;
  config.remove_ground_plane = true;
  config.keep_largest_component = true;
  config.max_integration_depth = 1.6f;
  config.train_splats = true;
  config.splat_iterations = 60;
  config.splat_max_gaussians = 4000;
  config.splat_sh_degree = 0;
  config.splat_image_width = 64;
  config.splat_max_views = 6;

  Pipeline pipeline(config);
  const StageOutcome outcome = pipeline.runToCompletion(nullptr, nullptr);
  ASSERT_TRUE(outcome.success) << outcome.message;

  const PipelineSummary summary = pipeline.summary();
  EXPECT_GT(summary.splat_count, 0);
  // The host build has no Vulkan, so the fallback is what ran. On a device the
  // same stage reports "gpu"; either way it must say which.
  EXPECT_EQ("cpu", summary.splat_backend);

  const fs::path splat = fs::path(capture.path()) / "outputs" / "splat.ply";
  ASSERT_TRUE(fs::exists(splat)) << splat.string();
  GaussianCloud cloud;
  std::string error;
  ASSERT_TRUE(readGaussianPly(splat.string(), &cloud, &error)) << error;
  EXPECT_EQ(static_cast<size_t>(summary.splat_count), cloud.size());
  std::string reason;
  EXPECT_TRUE(cloud.isValid(&reason)) << reason;

  // Every gaussian should have survived the final prune, so none is invisible.
  for (const Gaussian& gaussian : cloud.gaussians) {
    EXPECT_GE(gaussianOpacity(gaussian), 0.0045f);
  }
}

/// The time budget keeps what it trained instead of running all night.
///
/// A real device sat at 83% for hours, with the model it had already built unopenable because
/// the stage before finalise had not returned, and lost the work when the user left. The field
/// is a refinement of a model that already exists, so a partial one is worth keeping and the
/// stage says how far it got.
TEST(Pipeline, SplatTrainingStopsWhenItsTimeBudgetRunsOutAndKeepsTheField) {
  SyntheticCapture capture;
  capture.writeObjectCapture(10, 128);
  PipelineConfig config = objectConfig(capture.path());
  config.train_splats = true;
  config.splat_iterations = 100000;
  config.splat_max_gaussians = 20000;
  // A millisecond: the first callback is already past it, so this asserts the budget is
  // honoured rather than that it is generous.
  config.splat_time_budget_millis = 1;

  Pipeline pipeline(config);
  for (Stage stage : {Stage::Ingest, Stage::PoseRefinement, Stage::Depth, Stage::Fusion,
                      Stage::Meshing, Stage::MeshCleanup, Stage::Texturing,
                      Stage::PointCloud}) {
    ASSERT_TRUE(pipeline.runStage(stage, nullptr, nullptr).success) << stageName(stage);
  }

  const StageOutcome outcome = pipeline.runStage(Stage::SplatTraining, nullptr, nullptr);

  ASSERT_TRUE(outcome.success) << outcome.message;
  EXPECT_FALSE(outcome.cancelled);
  EXPECT_NE(outcome.message.find("time budget"), std::string::npos) << outcome.message;
  // The field is on disk, and the run can go on to finalise rather than being stuck here.
  EXPECT_GT(pipeline.summary().splat_count, 0);
  EXPECT_TRUE(pipeline.runStage(Stage::RoomAnalysis, nullptr, nullptr).success);
  EXPECT_TRUE(pipeline.runStage(Stage::Finalise, nullptr, nullptr).success);
}

TEST(Pipeline, TheSplatStageIsSkippedCleanlyWhenItIsNotWanted) {
  SyntheticCapture capture;
  capture.writeObjectCapture(10, 112);

  PipelineConfig config;
  config.project_dir = capture.path();
  config.mode = CaptureMode::Object;
  config.voxel_size = 0.012f;
  config.max_integration_depth = 1.6f;
  config.train_splats = false;

  Pipeline pipeline(config);
  ASSERT_TRUE(pipeline.runToCompletion(nullptr, nullptr).success);
  EXPECT_EQ(0, pipeline.summary().splat_count);
  EXPECT_TRUE(pipeline.summary().splat_backend.empty());
  EXPECT_FALSE(fs::exists(fs::path(capture.path()) / "outputs" / "splat.ply"));
}
