#include "parallax/pipeline/Pipeline.h"

#include "parallax/splat/SplatTrainer.h"
#include "parallax/splat/VulkanSplatTrainer.h"

#include <algorithm>
#include <limits>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

#include "parallax/exporters/MeshExport.h"
#include "parallax/features/Features.h"
#include "parallax/floorplan/PlaneDetection.h"
#include "parallax/io/ImageIo.h"
#include "parallax/io/JpegIo.h"
#include "parallax/depth/MultiViewStereo.h"
#include "parallax/io/Json.h"
#include "parallax/io/MeshReader.h"
#include "parallax/sfm/BundleAdjustment.h"
#include "parallax/sfm/Pnp.h"
#include "parallax/sfm/Triangulation.h"
#include "parallax/sfm/TwoView.h"
#include "parallax/texture/Texturing.h"

namespace parallax {
namespace fs = std::filesystem;
namespace {

/// Neighbouring views matched per reference frame during depth estimation.
constexpr int kMvsNeighbourCount = 4;

/// Depth-sweep resolution and sample count. Cost is quadratic in the first and
/// linear in the second, so the coarse pair is what a Fast scan uses.
constexpr int kMvsDownscale = 2;
constexpr int kMvsSamples = 48;
constexpr int kMvsFastDownscale = 3;
constexpr int kMvsFastSamples = 32;

/// How much of a pose-less capture has to be placed for the rest of the pipeline to work.
///
/// Below this the registered frames are a fragment of what the user photographed: depth
/// estimation has too few neighbours to correlate against, fusion sees one side of one thing,
/// and the run fails two stages after the cause with a message about overlap and texture.
/// Six tenths is what the real-photograph datasets here need to produce a closed surface.
constexpr double kMinRegisteredFraction = 0.6;

/// Matches a pair must keep after geometric verification to be worth building tracks from.
///
/// Before verification the same number counted raw matches, most of which could be wrong. A
/// verified match is one a single relative pose explains, so twenty of them is real overlap.
constexpr size_t kMinVerifiedPairMatches = 15;

/// Raw matches a pair needs before it is worth verifying. Not a quality judgement: the two-view
/// geometry makes that, and anything it can explain with fifteen matches is real overlap.
constexpr size_t kMinRawPairMatches = 18;

/// Long side, in pixels, that features are detected at whatever the source resolution.
///
/// About what the app's own capture writes, which is the resolution the rest of the tuning --
/// the FAST threshold, the descriptor patch, the match distances -- was chosen against. An
/// imported photograph can be any size, and matching fell apart on the large ones.
constexpr int kFeatureLongSide = 1600;

/// The fewest frames that can produce a surface rather than merely triangulate.
///
/// Two views are enough to recover depth along one line of sight, which is why the arithmetic
/// minimum is two, and not enough to see around anything: fusion then finds no closed surface
/// and the failure arrives six stages after the cause. Eight is the fewest that has produced a
/// usable model in the golden datasets here, and it is a number a user can act on.
constexpr size_t kMinKeyframesForReconstruction = 8;

/// Above this voxel size there is no point estimating depth finely: the volume
/// cannot represent the detail.
constexpr float kCoarseVoxelSizeM = 0.012f;

/// One frame as described by the manifest the capture layer writes.
struct FrameRecord {
  int index = 0;
  std::string image_file;
  std::string depth_file;
  CameraIntrinsics intrinsics;
  Pose pose;            ///< camera-from-world
  bool has_pose = false;
  double sharpness = 0.0;
  double exposure = 128.0;
  bool keyframe = false;
};

bool checkCancel(const CancelToken& cancel) {
  return cancel && cancel->load(std::memory_order_relaxed);
}

void report(const ProgressCallback& progress, Stage stage, double fraction,
            const std::string& detail) {
  if (!progress) return;
  Progress p;
  p.stage = stage;
  p.stage_fraction = std::max(0.0, std::min(1.0, fraction));
  // Overall progress is the weighted sum of completed stages plus this one's share, so the bar
  // moves at a roughly constant rate rather than stalling on the long stages.
  double before = 0.0;
  for (int s = 0; s < static_cast<int>(stage); ++s) {
    before += stageWeight(static_cast<Stage>(s));
  }
  p.overall_fraction = std::min(1.0, before + stageWeight(stage) * p.stage_fraction);
  p.detail = detail;
  progress(p);
}

/// Read a 32-bit float depth map written as width, height, then raw floats.
bool readDepthRaw(const std::string& path, DepthMap* out) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return false;
  if (bytes.size() < 8) return false;
  int32_t width = 0, height = 0;
  std::memcpy(&width, bytes.data(), 4);
  std::memcpy(&height, bytes.data() + 4, 4);
  if (width <= 0 || height <= 0) return false;
  const size_t expected = 8 + static_cast<size_t>(width) * height * sizeof(float);
  // Confidence, if present, follows as one byte per pixel.
  const size_t with_confidence = expected + static_cast<size_t>(width) * height;
  if (bytes.size() != expected && bytes.size() != with_confidence) return false;

  *out = DepthMap(width, height);
  std::memcpy(out->depth.data(), bytes.data() + 8,
              static_cast<size_t>(width) * height * sizeof(float));
  if (bytes.size() == with_confidence) {
    out->confidence.resize(static_cast<size_t>(width) * height);
    std::memcpy(out->confidence.data(), bytes.data() + expected, out->confidence.size());
  }
  return true;
}

/// Write a depth map in the same format `readDepthRaw` reads: width, height,
/// then the samples in metres, then one confidence byte each.
bool writeDepthRaw(const std::string& path, const DepthMap& depth) {
  if (depth.empty()) return false;
  const size_t samples = static_cast<size_t>(depth.width) * depth.height;
  std::vector<uint8_t> bytes(8 + samples * sizeof(float) + samples);
  const int32_t width = depth.width;
  const int32_t height = depth.height;
  std::memcpy(bytes.data(), &width, 4);
  std::memcpy(bytes.data() + 4, &height, 4);
  std::memcpy(bytes.data() + 8, depth.depth.data(), samples * sizeof(float));
  if (depth.confidence.size() == samples) {
    std::memcpy(bytes.data() + 8 + samples * sizeof(float), depth.confidence.data(), samples);
  } else {
    // No confidence means "fully trusted", which is what an absent array means
    // to `DepthMap::conf` as well.
    std::fill(bytes.begin() + 8 + samples * sizeof(float), bytes.end(), uint8_t{255});
  }
  return writeFileBytes(path, bytes);
}

/// The depth file an estimated frame is written to. `.mvs` rather than `.depth`
/// so an estimate is distinguishable from a sensor measurement on disk, which
/// matters when diagnosing a bad reconstruction from a pulled project directory.
std::string depthFileNameFor(int frame_index) {
  char name[32];
  std::snprintf(name, sizeof(name), "%06d.mvs", frame_index);
  return std::string(name);
}

Pose poseFromColumnMajor(const Json& array) {
  // The manifest stores world-from-camera column-major, matching ARCore and glTF.
  Mat4 world_from_camera = Mat4::Identity();
  if (array.isArray() && array.size() == 16) {
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        world_from_camera(row, column) = array.at(column * 4 + row).asDouble();
      }
    }
  }
  return Pose::fromWorldFromCamera(world_from_camera);
}

Json poseToColumnMajor(const Pose& pose) {
  const Mat4 world_from_camera = pose.worldFromCamera();
  Json array = Json::array();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      array.push(Json::number(world_from_camera(row, column)));
    }
  }
  return array;
}

}  // namespace

const char* stageName(Stage stage) {
  switch (stage) {
    case Stage::Ingest: return "ingest";
    case Stage::PoseRefinement: return "pose_refinement";
    case Stage::Depth: return "depth";
    case Stage::Fusion: return "fusion";
    case Stage::Meshing: return "meshing";
    case Stage::MeshCleanup: return "mesh_cleanup";
    case Stage::Texturing: return "texturing";
    case Stage::PointCloud: return "point_cloud";
    case Stage::SplatTraining: return "splat_training";
    case Stage::RoomAnalysis: return "room_analysis";
    case Stage::Finalise: return "finalise";
    default: return "unknown";
  }
}

double stageWeight(Stage stage) {
  switch (stage) {
    case Stage::Ingest: return 0.04;
    case Stage::PoseRefinement: return 0.14;
    case Stage::Depth: return 0.18;
    case Stage::Fusion: return 0.16;
    case Stage::Meshing: return 0.09;
    case Stage::MeshCleanup: return 0.05;
    case Stage::Texturing: return 0.12;
    case Stage::PointCloud: return 0.04;
    case Stage::SplatTraining: return 0.10;
    case Stage::RoomAnalysis: return 0.03;
    case Stage::Finalise: return 0.05;
    default: return 0.0;
  }
}

// ---------------------------------------------------------------------------- Impl

struct Pipeline::Impl {
  PipelineConfig config;
  PipelineSummary summary;
  std::vector<FrameRecord> frames;

  explicit Impl(PipelineConfig cfg) : config(std::move(cfg)) {
    std::error_code ec;
    fs::create_directories(checkpointDir(), ec);
    fs::create_directories(outputDir(), ec);
    loadSummary();
  }

  // ---- paths ------------------------------------------------------------
  fs::path projectDir() const { return fs::path(config.project_dir); }
  fs::path checkpointDir() const { return projectDir() / "checkpoints"; }
  fs::path outputDir() const { return projectDir() / "outputs"; }
  fs::path manifestPath() const { return projectDir() / "manifest.json"; }
  fs::path summaryPath() const { return checkpointDir() / "summary.json"; }

  fs::path checkpointPath(Stage stage) const {
    switch (stage) {
      case Stage::Ingest: return checkpointDir() / "ingest.json";
      case Stage::PoseRefinement: return checkpointDir() / "poses.json";
      case Stage::Depth: return checkpointDir() / "depth.json";
      case Stage::Fusion: return checkpointDir() / "tsdf.bin";
      case Stage::Meshing: return checkpointDir() / "mesh_raw.ply";
      case Stage::MeshCleanup: return checkpointDir() / "mesh_clean.ply";
      case Stage::Texturing: return checkpointDir() / "mesh_textured.ply";
      case Stage::PointCloud: return checkpointDir() / "cloud.ply";
    case Stage::SplatTraining: return checkpointDir() / "splat.ply";
      case Stage::RoomAnalysis: return checkpointDir() / "floorplan.json";
      case Stage::Finalise: return checkpointDir() / "finalise.json";
      default: return checkpointDir() / "unknown";
    }
  }

  bool hasCheckpoint(Stage stage) const {
    std::error_code ec;
    return fs::exists(checkpointPath(stage), ec);
  }

  // ---- summary persistence ----------------------------------------------
  void loadSummary() {
    std::string text;
    if (!readFileText(summaryPath().string(), &text)) return;
    Json json;
    if (!Json::parse(text, &json)) return;
    summary.frame_count = static_cast<int>(json.get("frame_count").asInt());
    summary.keyframe_count = static_cast<int>(json.get("keyframe_count").asInt());
    summary.registered_frame_count =
        static_cast<int>(json.get("registered_frame_count").asInt());
    summary.mean_reprojection_error_px = json.get("mean_reprojection_error_px").asDouble();
    summary.triangle_count = json.get("triangle_count").asInt();
    summary.vertex_count = json.get("vertex_count").asInt();
    summary.point_count = json.get("point_count").asInt();
    summary.splat_count = json.get("splat_count").asInt();
    summary.splat_backend = json.get("splat_backend").asString();
    summary.surface_area_m2 = json.get("surface_area_m2").asDouble();
    summary.volume_m3 = json.get("volume_m3").asDouble();
    summary.watertight = json.get("watertight").asBool();
    summary.floor_area_m2 = json.get("floor_area_m2").asDouble();
    summary.ceiling_height_m = json.get("ceiling_height_m").asDouble();
    summary.has_floor_plan = json.get("has_floor_plan").asBool();
    summary.has_texture = json.get("has_texture").asBool();
    summary.scale_factor = json.get("scale_factor").asDouble(1.0);
    const Json& bounds = json.get("bounds");
    if (bounds.isArray() && bounds.size() == 6) {
      summary.bounds.min = Vec3(bounds.at(0).asDouble(), bounds.at(1).asDouble(),
                                bounds.at(2).asDouble());
      summary.bounds.max = Vec3(bounds.at(3).asDouble(), bounds.at(4).asDouble(),
                                bounds.at(5).asDouble());
    }
  }

  void saveSummary() const {
    Json json = Json::object();
    json.set("frame_count", Json::integer(summary.frame_count));
    json.set("keyframe_count", Json::integer(summary.keyframe_count));
    json.set("registered_frame_count", Json::integer(summary.registered_frame_count));
    json.set("mean_reprojection_error_px", Json::number(summary.mean_reprojection_error_px));
    json.set("triangle_count", Json::integer(summary.triangle_count));
    json.set("vertex_count", Json::integer(summary.vertex_count));
    json.set("point_count", Json::integer(summary.point_count));
    json.set("splat_count", Json::integer(summary.splat_count));
    json.set("splat_backend", Json::string(summary.splat_backend));
    json.set("surface_area_m2", Json::number(summary.surface_area_m2));
    json.set("volume_m3", Json::number(summary.volume_m3));
    json.set("watertight", Json::boolean(summary.watertight));
    json.set("floor_area_m2", Json::number(summary.floor_area_m2));
    json.set("ceiling_height_m", Json::number(summary.ceiling_height_m));
    json.set("has_floor_plan", Json::boolean(summary.has_floor_plan));
    json.set("has_texture", Json::boolean(summary.has_texture));
    json.set("scale_factor", Json::number(summary.scale_factor));
    if (summary.bounds.valid()) {
      Json bounds = Json::array();
      for (int i = 0; i < 3; ++i) bounds.push(Json::number(summary.bounds.min[i]));
      for (int i = 0; i < 3; ++i) bounds.push(Json::number(summary.bounds.max[i]));
      json.set("bounds", std::move(bounds));
    }
    writeFileText(summaryPath().string(), json.dump(2));
  }

  // ---- frame access -----------------------------------------------------
  /// Parse `manifest.json` into [frames]. Cheap enough to repeat per stage, and doing so keeps
  /// stages independent so any one of them can run after a cold resume.
  bool loadFrames() {
    if (!frames.empty()) return true;
    std::string text;
    if (!readFileText(manifestPath().string(), &text)) return false;
    Json manifest;
    std::string error;
    if (!Json::parse(text, &manifest, &error)) return false;

    const Json& list = manifest.get("frames");
    if (!list.isArray()) return false;
    for (size_t i = 0; i < list.size(); ++i) {
      const Json& entry = list.at(i);
      FrameRecord record;
      record.index = static_cast<int>(entry.get("index").asInt(static_cast<int64_t>(i)));
      record.image_file = entry.get("image").asString();
      record.depth_file = entry.get("depth").asString();
      record.sharpness = entry.get("sharpness").asDouble(1.0);
      record.exposure = entry.get("exposure").asDouble(128.0);
      record.keyframe = entry.get("keyframe").asBool(true);

      const Json& k = entry.get("intrinsics");
      record.intrinsics.fx = k.get("fx").asDouble();
      record.intrinsics.fy = k.get("fy").asDouble();
      record.intrinsics.cx = k.get("cx").asDouble();
      record.intrinsics.cy = k.get("cy").asDouble();
      record.intrinsics.k1 = k.get("k1").asDouble(0.0);
      record.intrinsics.k2 = k.get("k2").asDouble(0.0);
      record.intrinsics.width = static_cast<int>(k.get("width").asInt());
      record.intrinsics.height = static_cast<int>(k.get("height").asInt());

      if (entry.has("pose")) {
        record.pose = poseFromColumnMajor(entry.get("pose"));
        record.has_pose = true;
      }
      if (record.image_file.empty() || !record.intrinsics.valid()) continue;
      frames.push_back(std::move(record));
    }
    return !frames.empty();
  }

  /// Keyframes only, capped at the quality preset's limit, spread evenly over the capture.
  std::vector<int> selectKeyframes() const {
    std::vector<int> candidates;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].keyframe) candidates.push_back(static_cast<int>(i));
    }
    if (candidates.empty()) {
      for (size_t i = 0; i < frames.size(); ++i) candidates.push_back(static_cast<int>(i));
    }
    const int limit = std::max(2, config.max_keyframes);
    if (static_cast<int>(candidates.size()) <= limit) return candidates;

    // Decimate evenly rather than truncating: taking the first N frames of an orbit would cover
    // only part of the subject.
    std::vector<int> selected;
    selected.reserve(limit);
    for (int i = 0; i < limit; ++i) {
      const size_t index =
          static_cast<size_t>(static_cast<double>(i) * (candidates.size() - 1) / (limit - 1));
      selected.push_back(candidates[index]);
    }
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    return selected;
  }

  /**
   * Load a frame, in whichever format it was written.
   *
   * Capture writes JPEG, because 160 frames is 250 MB of JPEG against 1.3 GB of
   * PNG and the phone's encoder produces it for free from the camera's YUV
   * planes. The engine's own tests and its exports use PNG. Sniffing the magic
   * bytes rather than the extension means a frame renamed by a file manager, or
   * a bundle written by a different version, still loads.
   */
  bool loadImage(const FrameRecord& frame, ColorBuffer* out) const {
    std::vector<uint8_t> bytes;
    const fs::path path = projectDir() / "frames" / frame.image_file;
    if (!readFileBytes(path.string(), &bytes)) return false;
    if (looksLikeJpeg(bytes)) return decodeJpeg(bytes, out);
    return decodePng(bytes, out);
  }

  /**
   * Read a frame's depth map, measured or estimated.
   *
   * The fallback is not belt and braces. The depth stage writes the maps it estimates to
   * `depth/` under a name derived from the frame index, and records them on the in-memory
   * frame record -- but the manifest on disk is the app's record of what the camera captured
   * and never learns about them. So any later stage reached in a fresh process, which is every
   * resume and every retry, loaded the manifest, found no depth file, and fused nothing. The
   * file is on disk and its name is a function of the index, so the name is enough: looking
   * for it makes the depth stage's work survive the process that did it.
   */
  bool loadDepth(const FrameRecord& frame, DepthMap* out) const {
    if (!frame.depth_file.empty()) {
      const fs::path named = projectDir() / "depth" / frame.depth_file;
      if (readDepthRaw(named.string(), out)) return true;
    }
    const fs::path estimated = projectDir() / "depth" / depthFileNameFor(frame.index);
    return readDepthRaw(estimated.string(), out);
  }

  // ---- the stages -------------------------------------------------------
  StageOutcome runIngest(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runPoseRefinement(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runDepth(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runFusion(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runMeshing(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runMeshCleanup(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runTexturing(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runPointCloud(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runSplatTraining(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runRoomAnalysis(const ProgressCallback& progress, const CancelToken& cancel);
  StageOutcome runFinalise(const ProgressCallback& progress, const CancelToken& cancel);

  /// Refined poses, written by the pose stage and read by everything after it.
  bool loadRefinedPoses(std::vector<int>* keyframes, std::vector<Pose>* poses,
                        std::vector<Vec3>* sparse_points = nullptr);

  int estimateMissingDepth(const std::vector<int>& keyframes, const std::vector<Pose>& poses,
                           const std::vector<Vec3>& sparse_points,
                           const std::vector<uint8_t>& has_depth,
                           const ProgressCallback& progress, const CancelToken& cancel);

  /**
   * The rotation mesh cleanup applied to level the model against gravity.
   *
   * It has to be persisted and re-applied by every later stage, because levelling rotates the
   * *world*: the mesh moves but the camera poses do not, and a texturing pass that uses
   * unrotated poses against a rotated mesh finds that no view can see any triangle at all.
   * Identity when no floor was detected.
   */
  Mat3 loadLevelRotation() const;
  void saveLevelRotation(const Mat3& rotation) const;

  /// Transform camera poses into the levelled world frame.
  static void levelPoses(const Mat3& rotation, std::vector<Pose>* poses) {
    if (rotation.isApprox(Mat3::Identity(), 1e-12)) return;
    // A point moves as p' = R p, so for p_cam = R_cw p + t to hold in the new frame the
    // rotation becomes R_cw R^T while the translation is unchanged.
    for (Pose& pose : *poses) {
      pose.R = pose.R * rotation.transpose();
      pose.normalize();
    }
  }
};

Mat3 Pipeline::Impl::loadLevelRotation() const {
  std::string text;
  if (!readFileText((checkpointDir() / "level.json").string(), &text)) return Mat3::Identity();
  Json json;
  if (!Json::parse(text, &json)) return Mat3::Identity();
  const Json& values = json.get("rotation");
  if (!values.isArray() || values.size() != 9) return Mat3::Identity();
  Mat3 rotation;
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      rotation(row, column) = values.at(row * 3 + column).asDouble();
    }
  }
  if (!rotation.allFinite()) return Mat3::Identity();
  return orthonormalize(rotation);
}

void Pipeline::Impl::saveLevelRotation(const Mat3& rotation) const {
  Json values = Json::array();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      values.push(Json::number(rotation(row, column)));
    }
  }
  Json json = Json::object();
  json.set("rotation", std::move(values));
  writeFileText((checkpointDir() / "level.json").string(), json.dump(2));
}

// ---------------------------------------------------------------------- stages

bool Pipeline::Impl::loadRefinedPoses(std::vector<int>* keyframes, std::vector<Pose>* poses,
                                      std::vector<Vec3>* sparse_points) {
  std::string text;
  if (!readFileText(checkpointPath(Stage::PoseRefinement).string(), &text)) return false;
  Json json;
  if (!Json::parse(text, &json)) return false;
  const Json& list = json.get("keyframes");
  if (!list.isArray() || list.size() == 0) return false;

  keyframes->clear();
  poses->clear();
  for (size_t i = 0; i < list.size(); ++i) {
    const Json& entry = list.at(i);
    const int index = static_cast<int>(entry.get("frame").asInt(-1));
    if (index < 0 || index >= static_cast<int>(frames.size())) return false;
    keyframes->push_back(index);
    poses->push_back(poseFromColumnMajor(entry.get("pose")));
  }
  if (sparse_points != nullptr) {
    sparse_points->clear();
    const Json& points = json.get("points");
    if (points.isArray()) {
      for (size_t i = 0; i < points.size(); ++i) {
        const Json& entry = points.at(i);
        if (!entry.isArray() || entry.size() < 3) continue;
        sparse_points->push_back(Vec3(entry.at(0).asDouble(), entry.at(1).asDouble(),
                                      entry.at(2).asDouble()));
      }
    }
  }
  summary.scale_factor = json.get("scale_factor").asDouble(1.0);
  return true;
}

StageOutcome Pipeline::Impl::runIngest(const ProgressCallback& progress,
                                       const CancelToken& cancel) {
  report(progress, Stage::Ingest, 0.0, "Reading the capture manifest");
  frames.clear();
  if (!loadFrames()) {
    return StageOutcome{false, false, "no readable frames in the project manifest"};
  }
  const std::vector<int> keyframes = selectKeyframes();
  // Two is the arithmetic minimum for triangulation and nowhere near enough for a surface: a
  // pair of views sees one side of one thing, so fusion finds no closed surface and the run
  // dies six stages after the problem, saying the depth and the poses disagree. Refused here,
  // where the number is still something the user can act on.
  if (keyframes.size() < kMinKeyframesForReconstruction) {
    return StageOutcome{false, false,
                        "only " + std::to_string(keyframes.size()) +
                            " usable frames; reconstruction needs at least " +
                            std::to_string(kMinKeyframesForReconstruction) +
                            ", taken from different positions around the subject"};
  }
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  // Verify that every selected keyframe's image actually decodes. Finding a truncated PNG now
  // is far better than failing three stages later with a confusing error.
  int readable = 0;
  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    ColorBuffer image;
    if (loadImage(frames[keyframes[i]], &image) && !image.empty()) ++readable;
    report(progress, Stage::Ingest, static_cast<double>(i + 1) / keyframes.size(),
           "Checking frames");
  }
  if (readable < 2) {
    return StageOutcome{false, false, "the capture's image files could not be read"};
  }

  summary.frame_count = static_cast<int>(frames.size());
  summary.keyframe_count = static_cast<int>(keyframes.size());

  Json json = Json::object();
  json.set("frame_count", Json::integer(summary.frame_count));
  json.set("keyframe_count", Json::integer(summary.keyframe_count));
  json.set("readable_keyframes", Json::integer(readable));
  Json list = Json::array();
  for (int index : keyframes) list.push(Json::integer(index));
  json.set("keyframes", std::move(list));
  if (!writeFileText(checkpointPath(Stage::Ingest).string(), json.dump(2))) {
    return StageOutcome{false, false, "could not write the ingest checkpoint"};
  }
  saveSummary();
  return StageOutcome{true, false, "read " + std::to_string(readable) + " keyframes"};
}

StageOutcome Pipeline::Impl::runPoseRefinement(const ProgressCallback& progress,
                                               const CancelToken& cancel) {
  if (!loadFrames()) return StageOutcome{false, false, "frames unavailable"};
  report(progress, Stage::PoseRefinement, 0.0, "Selecting keyframes");

  std::vector<int> keyframes;
  {
    std::string text;
    Json json;
    if (readFileText(checkpointPath(Stage::Ingest).string(), &text) &&
        Json::parse(text, &json)) {
      const Json& list = json.get("keyframes");
      for (size_t i = 0; i < list.size(); ++i) {
        keyframes.push_back(static_cast<int>(list.at(i).asInt()));
      }
    }
  }
  if (keyframes.empty()) keyframes = selectKeyframes();
  if (keyframes.size() < 2) return StageOutcome{false, false, "too few keyframes"};

  // Detect features on every keyframe. This is the expensive part of the stage, so progress is
  // reported per frame.
  std::vector<FeatureSet> features(keyframes.size());
  std::vector<CameraIntrinsics> intrinsics(keyframes.size());
  FeatureParams feature_params;
  feature_params.max_features = config.has_device_poses ? 1400 : 2600;
  // A smaller border keeps corners near the frame edge, which matter disproportionately for
  // wide-baseline matching because they are the parts two distant views still share.
  feature_params.border = 22;

  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    ColorBuffer image;
    if (!loadImage(frames[keyframes[i]], &image)) continue;

    // Features are found at a fixed resolution, whatever the camera's.
    //
    // The descriptor samples a fixed-size patch around each corner, so what it describes
    // depends entirely on how much of the scene a pixel covers. On a frame the app captured
    // that is about 2,000 px across and the patch spans real structure; on a twelve-megapixel
    // photograph from a gallery it spans a few bricks' worth of noise, and the descriptors
    // stop matching. Twenty-four consecutive photographs of a building registered four frames
    // because of this -- not for want of overlap, which is what the failure said.
    //
    // Downscaled with a box filter rather than point-sampled: point sampling keeps the sensor
    // noise at full amplitude, which is the thing the detector then finds corners in.
    const int long_side = std::max(image.width, image.height);
    double feature_scale = 1.0;
    const GrayBuffer gray = [&] {
      if (long_side <= kFeatureLongSide) return toGray(image);
      const int target_width = image.width >= image.height
                                   ? kFeatureLongSide
                                   : std::max(1, image.width * kFeatureLongSide / image.height);
      const ColorBuffer reduced = downscaleToWidth(image, target_width);
      feature_scale = static_cast<double>(image.width) / std::max(1, reduced.width);
      return toGray(reduced);
    }();
    features[i] = detectAndDescribe(gray.view(), feature_params);
    // Back to full-resolution coordinates, so the intrinsics, the matches and everything
    // downstream stay in the one frame of reference.
    if (feature_scale != 1.0) {
      for (Keypoint& keypoint : features[i].keypoints) keypoint.pt *= feature_scale;
    }
    intrinsics[i] = frames[keyframes[i]].intrinsics.width == image.width
                        ? frames[keyframes[i]].intrinsics
                        : frames[keyframes[i]].intrinsics.scaledTo(image.width, image.height);
    report(progress, Stage::PoseRefinement, 0.45 * (i + 1) / keyframes.size(),
           "Finding features");
  }

  // Match within a sliding window plus a loop closure, rather than exhaustively.
  //
  // The window is wider when there are no device poses, because then the matches are the *only*
  // thing tying the reconstruction together: a track that spans more frames is better
  // conditioned, and more pairs means longer tracks. With device poses a narrow window is
  // enough, since the poses already supply the global structure.
  struct PairMatches {
    int a, b;
    std::vector<Match> matches;
    /// Two-view geometry over this pair's matches, computed once and kept.
    TwoViewGeometry geometry;
    bool verified = false;
  };
  std::vector<PairMatches> pairs;
  const int window = config.has_device_poses ? 3 : 6;
  for (size_t i = 0; i < keyframes.size(); ++i) {
    for (int offset = 1; offset <= window; ++offset) {
      const size_t j = i + offset;
      if (j >= keyframes.size()) break;
      if (features[i].empty() || features[j].empty()) continue;
      PairMatches pair;
      pair.a = static_cast<int>(i);
      pair.b = static_cast<int>(j);
      // Binary descriptors lose discriminative power as the viewpoint change grows, so for
      // the pose-less path the ratio test is relaxed slightly and a larger Hamming distance is
      // allowed. Cross-checking is kept: it is what stops the extra candidates becoming
      // outliers, and RANSAC downstream removes the rest.
      MatchParams match_params;
      if (!config.has_device_poses) {
        match_params.max_distance = 80;
        match_params.ratio = 0.88;
      }
      pair.matches = matchFeatures(features[i], features[j], match_params);
      // A low bar, because this is only about not wasting a verification on a pair with
      // nothing in it. Deciding which pairs are real used to happen here, on the raw match
      // count, and it threw away pairs that would have verified while keeping pairs that were
      // mostly false matches from repeated texture. The geometry below decides now.
      if (pair.matches.size() >= kMinRawPairMatches) pairs.push_back(std::move(pair));
    }
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
  }
  // Close the loop for an orbit capture.
  if (keyframes.size() > 8 && !features.front().empty() && !features.back().empty()) {
    PairMatches pair;
    pair.a = 0;
    pair.b = static_cast<int>(keyframes.size() - 1);
    pair.matches = matchFeatures(features.front(), features.back());
    if (pair.matches.size() >= kMinRawPairMatches) pairs.push_back(std::move(pair));
  }
  report(progress, Stage::PoseRefinement, 0.6, "Matching frames");

  // ---- geometric verification -------------------------------------------
  //
  // Every pair's matches are cut down to the ones a single relative pose explains, and pairs
  // that no pose explains are dropped. The descriptor matcher cannot tell a real
  // correspondence from a plausible one, and the things people photograph are full of repeated
  // detail -- the windows of a facade, the tiles of a floor, the slats of a blind. Those
  // matches pass the ratio test, pass cross-checking, and are wrong.
  //
  // Leaving them in was the reason registration stalled. They fuse distinct world points into
  // one track, so the triangulated point is in neither place; a frame is then offered two
  // hundred correspondences to points that are not where they claim, and no pose fits more
  // than a handful. Eleven real photographs of a building registered three frames, and the
  // failure two stages later blamed overlap -- on a set with 3,636 tracks.
  //
  // The geometry was already being computed to choose the initialisation pair, and its inlier
  // mask thrown away. This costs nothing new.
  {
    std::vector<PairMatches> verified;
    verified.reserve(pairs.size());
    int dropped_matches = 0;
    for (PairMatches& pair : pairs) {
      if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
      std::vector<PointPair> correspondences;
      correspondences.reserve(pair.matches.size());
      for (const Match& match : pair.matches) {
        correspondences.push_back(PointPair{features[pair.a].keypoints[match.query].pt,
                                            features[pair.b].keypoints[match.train].pt});
      }
      pair.geometry = estimateTwoView(correspondences, intrinsics[pair.a], intrinsics[pair.b]);
      pair.verified = pair.geometry.success;
      if (!pair.verified) {
        dropped_matches += static_cast<int>(pair.matches.size());
        continue;
      }
      std::vector<Match> kept;
      kept.reserve(pair.geometry.inlier_count);
      for (size_t m = 0; m < pair.matches.size(); ++m) {
        if (m < pair.geometry.inliers.size() && pair.geometry.inliers[m]) {
          kept.push_back(pair.matches[m]);
        }
      }
      dropped_matches += static_cast<int>(pair.matches.size() - kept.size());
      pair.matches = std::move(kept);
      // The same evidence threshold as before verification, now applied to matches that mean
      // something: a pair whose inliers are this few is a pair with no reliable overlap.
      if (pair.matches.size() >= kMinVerifiedPairMatches) verified.push_back(std::move(pair));
    }
    pairs = std::move(verified);
    report(progress, Stage::PoseRefinement, 0.62, "Checking matches");
  }
  // Fatal only without device poses. With them the frames are already placed and the pairs
  // are there to refine the placement, so having none means there is nothing to refine -- not
  // that the capture is unusable. A scan of a plain wall can legitimately verify no pair at
  // all, and failing it here would reject a capture the device had tracked perfectly.
  if (pairs.empty() && !config.has_device_poses) {
    return StageOutcome{false, false,
                        "no two frames could be related to each other; the photographs may be"
                        " of different things, or taken from too far apart"};
  }

  // Seed poses. With device poses we refine them; without, we initialise from two-view geometry
  // and grow by PnP.
  std::vector<Pose> poses(keyframes.size());
  std::vector<uint8_t> registered(keyframes.size(), 0);
  double scale_factor = 1.0;

  if (config.has_device_poses) {
    int with_pose = 0;
    for (size_t i = 0; i < keyframes.size(); ++i) {
      if (frames[keyframes[i]].has_pose) {
        poses[i] = frames[keyframes[i]].pose;
        registered[i] = 1;
        ++with_pose;
      }
    }
    if (with_pose < 2) {
      return StageOutcome{false, false,
                          "the capture claimed device poses but none were usable"};
    }
  } else {
    // Initialise from the pair with the most inliers that is *not* degenerate. A planar scene
    // or a pure rotation admits infinitely many essential matrices, so initialising from such a
    // pair produces a meaningless baseline.
    int best_pair = -1;
    int best_inliers = 0;
    TwoViewGeometry best_geometry;
    for (size_t p = 0; p < pairs.size(); ++p) {
      const PairMatches& pair = pairs[p];
      if (!pair.verified) continue;
      if (pair.geometry.planarity > 0.85) continue;  // degenerate for pose recovery
      if (pair.geometry.inlier_count > best_inliers) {
        best_inliers = pair.geometry.inlier_count;
        best_pair = static_cast<int>(p);
        best_geometry = pair.geometry;
      }
    }
    if (best_pair < 0) {
      return StageOutcome{false, false,
                          "no pair of frames had enough overlap, or every pair was planar"};
    }
    poses[pairs[best_pair].a] = Pose::Identity();
    poses[pairs[best_pair].b] = best_geometry.relative_pose;
    registered[pairs[best_pair].a] = 1;
    registered[pairs[best_pair].b] = 1;
    // Monocular reconstruction has no metric scale; the unit baseline is arbitrary and the user
    // supplies a real dimension later in the viewer.
    scale_factor = 0.0;
  }

  // Triangulate tracks between registered pairs, then grow the registration by PnP.
  BundleProblem problem;
  problem.poses = poses;
  problem.intrinsics = intrinsics;

  // ---- tracks -----------------------------------------------------------
  //
  // A feature track is a set of (frame, feature) observations of one world point, built by
  // union-find over the pairwise matches. Two guards matter:
  //   * a track must never contain two features from the same frame -- that means two matches
  //     disagreed, and merging anyway fuses two distinct world points into one;
  //   * tracks shorter than two frames carry no information and are dropped.
  // Chaining matches naively without the first guard is the single biggest source of garbage
  // structure in a hand-rolled SfM front end.
  std::vector<std::pair<int, int>> observations_flat;  // (keyframe slot, feature index)
  std::map<std::pair<int, int>, int> observation_id;
  const auto observationId = [&](int frame, int feature) {
    const std::pair<int, int> key{frame, feature};
    const auto it = observation_id.find(key);
    if (it != observation_id.end()) return it->second;
    const int id = static_cast<int>(observations_flat.size());
    observations_flat.push_back(key);
    observation_id.emplace(key, id);
    return id;
  };

  std::vector<int> parent;
  const auto find = [&parent](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];  // path halving
      x = parent[x];
    }
    return x;
  };

  for (const PairMatches& pair : pairs) {
    for (const Match& match : pair.matches) {
      observationId(pair.a, match.query);
      observationId(pair.b, match.train);
    }
  }
  parent.resize(observations_flat.size());
  for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
  // Frames already present in each set, to enforce the one-feature-per-frame rule.
  std::vector<std::vector<int>> set_frames(observations_flat.size());
  for (size_t i = 0; i < observations_flat.size(); ++i) {
    set_frames[i].push_back(observations_flat[i].first);
  }

  for (const PairMatches& pair : pairs) {
    for (const Match& match : pair.matches) {
      const int a = find(observationId(pair.a, match.query));
      const int b = find(observationId(pair.b, match.train));
      if (a == b) continue;
      // Refuse the merge if it would put two features of one frame in the same track.
      bool conflict = false;
      for (int frame_a : set_frames[a]) {
        for (int frame_b : set_frames[b]) {
          if (frame_a == frame_b) {
            conflict = true;
            break;
          }
        }
        if (conflict) break;
      }
      if (conflict) continue;
      // Union by size, keeping the frame list on the new root.
      const int root = set_frames[a].size() >= set_frames[b].size() ? a : b;
      const int child = root == a ? b : a;
      parent[child] = root;
      set_frames[root].insert(set_frames[root].end(), set_frames[child].begin(),
                              set_frames[child].end());
      set_frames[child].clear();
      set_frames[child].shrink_to_fit();
    }
  }

  std::map<int, int> track_index_of_root;
  std::vector<std::vector<Observation>> tracks;
  for (size_t i = 0; i < observations_flat.size(); ++i) {
    const int root = find(static_cast<int>(i));
    auto it = track_index_of_root.find(root);
    if (it == track_index_of_root.end()) {
      it = track_index_of_root.emplace(root, static_cast<int>(tracks.size())).first;
      tracks.emplace_back();
    }
    const auto& [frame, feature] = observations_flat[i];
    tracks[it->second].push_back(
        Observation{frame, it->second, features[frame].keypoints[feature].pt, 1.0f});
  }
  // Drop tracks too short to triangulate.
  tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                              [](const std::vector<Observation>& track) {
                                return track.size() < 2;
                              }),
               tracks.end());
  for (size_t t = 0; t < tracks.size(); ++t) {
    for (Observation& observation : tracks[t]) observation.point = static_cast<int>(t);
  }
  report(progress, Stage::PoseRefinement, 0.7, "Building tracks");

  // ---- incremental registration -----------------------------------------
  //
  // Only needed when no device poses were supplied. Greedy best-next-view: triangulate what the
  // registered set can see, then register whichever remaining frame has the most 2D-3D
  // correspondences, and repeat. Choosing the *best* next view rather than sweeping in index
  // order matters, because a frame with marginal overlap registered early produces a bad pose
  // that then poisons the points it helps triangulate.
  if (!config.has_device_poses) {
    std::vector<Vec3> points(tracks.size(), Vec3::Zero());
    std::vector<uint8_t> valid(tracks.size(), 0);
    // Frames whose PnP has already failed, so the loop does not retry them forever.
    //
    // This must be a *separate* array, not a sentinel value in `registered`: every stage that
    // consumes `registered` treats it as a boolean, so a non-zero "failed" marker would make a
    // frame count as registered and have its identity pose fed into triangulation, quietly
    // corrupting every point after the first failure.
    std::vector<uint8_t> attempted(keyframes.size(), 0);

    const auto retriangulate = [&]() {
      int count = 0;
      for (size_t t = 0; t < tracks.size(); ++t) {
        std::vector<Observation> usable;
        for (const Observation& observation : tracks[t]) {
          if (registered[observation.camera]) usable.push_back(observation);
        }
        if (usable.size() < 2) continue;
        const TriangulationResult tri =
            triangulateMultiView(poses, intrinsics, usable, 0.8, 8.0);
        if (tri.valid) {
          points[t] = tri.point;
          valid[t] = 1;
          ++count;
        }
      }
      return count;
    };
    retriangulate();

    // Bounded by attempts rather than by rounds, because a frame that failed is allowed back.
    // Four tries per frame is enough for the set to grow around a hard one and come back to it,
    // and is a ceiling on the work rather than a target: progress clears the failures, so a
    // capture that registers cleanly makes exactly one attempt per frame.
    const size_t max_attempts = keyframes.size() * 4;
    for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
      if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

      // Gather candidate correspondences for every unregistered frame.
      int best_frame = -1;
      std::vector<PointCorrespondence> best_correspondences;
      for (size_t i = 0; i < keyframes.size(); ++i) {
        if (registered[i] || attempted[i]) continue;
        std::vector<PointCorrespondence> correspondences;
        for (size_t t = 0; t < tracks.size(); ++t) {
          if (!valid[t]) continue;
          for (const Observation& observation : tracks[t]) {
            if (observation.camera == static_cast<int>(i)) {
              correspondences.push_back(PointCorrespondence{points[t], observation.pixel});
              break;
            }
          }
        }
        if (correspondences.size() > best_correspondences.size()) {
          best_correspondences = std::move(correspondences);
          best_frame = static_cast<int>(i);
        }
      }
      // Six is the minimum the DLT needs; below that there is nothing to try.
      if (best_frame < 0 || best_correspondences.size() < 8) break;

      RansacOptions pnp_options;
      pnp_options.inlier_threshold = 5.0;
      pnp_options.max_iterations = 2500;
      PnpResult pnp = pnpRansac(best_correspondences, intrinsics[best_frame], pnp_options);

      // Non-linear fallback, seeded from every frame already placed.
      //
      // The DLT behind the RANSAC is a linear formulation and needs points that are not
      // coplanar. A great deal of what people photograph is: a wall, a facade, a floor, a
      // table top. On eleven real photographs of a building the RANSAC returned zero inliers
      // on every single frame, because every minimal sample it drew was degenerate -- while
      // each of those frames had two hundred good correspondences sitting there.
      //
      // Gauss-Newton on the reprojection error has no such degeneracy; it needs a starting
      // pose. Seeding from the *nearest* placed frame worked only for a frame adjacent to one,
      // and the camera moves between photographs: three frames away the seed was wrong by
      // enough that the refinement converged to a pose 600 px out. Every placed pose is tried
      // instead and the one with the most inliers wins, which costs a Gauss-Newton per placed
      // frame -- nothing next to the RANSAC it is replacing.
      if (!pnp.success || pnp.inlier_count < 8) {
        const auto scorePose = [&](const Pose& pose, double* inlier_rmse) {
          int inliers = 0;
          double sum_sq = 0.0;
          for (const PointCorrespondence& c : best_correspondences) {
            Vec2 projected;
            if (!intrinsics[best_frame].project(pose * c.world, &projected)) continue;
            const double error = (projected - c.pixel).norm();
            if (error > pnp_options.inlier_threshold) continue;
            ++inliers;
            sum_sq += error * error;
          }
          // Measured over the inliers only. The RMSE over everything is dominated by the
          // outliers the Huber loss deliberately ignored, so gating on it rejects good poses;
          // gating on the inlier count alone accepts poses that fit a subset by coincidence.
          *inlier_rmse = inliers > 0 ? std::sqrt(sum_sq / inliers) : 1e9;
          return inliers;
        };

        Pose best_pose;
        int best_inliers = 0;
        double best_rmse = 1e9;
        for (size_t j = 0; j < keyframes.size(); ++j) {
          if (!registered[j]) continue;
          Pose seeded = poses[j];
          double unused_rmse = 0.0;
          if (!refinePoseGaussNewton(best_correspondences, intrinsics[best_frame], &seeded, 40,
                                     pnp_options.inlier_threshold, &unused_rmse)) {
            continue;
          }
          double inlier_rmse = 1e9;
          const int inliers = scorePose(seeded, &inlier_rmse);
          if (inliers > best_inliers) {
            best_inliers = inliers;
            best_rmse = inlier_rmse;
            best_pose = seeded;
          }
        }

        // Both an absolute count and a share of what was offered, so a pose that merely fits a
        // handful of points is not accepted.
        if (best_inliers >= 12 &&
            best_inliers * 100 >= static_cast<int>(best_correspondences.size()) * 35 &&
            best_rmse <= pnp_options.inlier_threshold) {
          pnp.pose = best_pose;
          pnp.inlier_count = best_inliers;
          pnp.rmse_px = best_rmse;
          pnp.success = true;
        }
      }

      if (!pnp.success || pnp.inlier_count < 8) {
        attempted[best_frame] = 1;
        continue;
      }
      poses[best_frame] = pnp.pose;
      registered[best_frame] = 1;
      retriangulate();
      // A frame that failed is given another chance now that the registered set has grown.
      //
      // Without this, failing once was permanent, and failing once is normal: the first few
      // registrations have only the initial pair's points to work from, so a frame that sees a
      // different part of the scene has almost nothing to match against and is excluded before
      // the points it needs exist. On eleven real photographs of a building that left three
      // registered and eight locked out, and the run died two stages later complaining about
      // overlap -- on a set with 46 matched pairs and 3,636 tracks.
      std::fill(attempted.begin(), attempted.end(), 0);

      // Local bundle adjustment after every registration.
      //
      // Without this, each new pose is fitted to points triangulated from slightly wrong
      // earlier poses, so the error compounds: by the fifth or sixth frame the points are bad
      // enough that no further frame can be registered at all, and growth simply stops. A
      // short solve over everything registered so far keeps poses and structure consistent,
      // which is what lets the sequence keep extending.
      {
        BundleProblem local;
        local.poses = poses;
        local.intrinsics = intrinsics;
        std::vector<int> point_of_track(tracks.size(), -1);
        for (size_t t = 0; t < tracks.size(); ++t) {
          if (!valid[t]) continue;
          int seen = 0;
          for (const Observation& observation : tracks[t]) {
            if (registered[observation.camera]) ++seen;
          }
          if (seen < 2) continue;
          point_of_track[t] = static_cast<int>(local.points.size());
          local.points.push_back(points[t]);
        }
        for (size_t t = 0; t < tracks.size(); ++t) {
          if (point_of_track[t] < 0) continue;
          for (Observation observation : tracks[t]) {
            if (!registered[observation.camera]) continue;
            observation.point = point_of_track[t];
            local.observations.push_back(observation);
          }
        }
        local.constant_poses.assign(poses.size(), 0);
        for (size_t j = 0; j < registered.size(); ++j) {
          if (!registered[j]) local.constant_poses[j] = 1;
        }
        if (local.valid() && local.points.size() >= 20) {
          BundleOptions local_options;
          local_options.max_iterations = 12;
          local_options.fix_scale = true;
          local_options.max_dense_cameras = 400;
          optimizeBundle(&local, local_options);
          // Discard observations the solve could not explain, so a bad match does not get a
          // second chance to drag the next registration off.
          pruneOutlierObservations(&local, 4.0);
          if (local.valid()) {
            optimizeBundle(&local, local_options);
            poses = local.poses;
            for (size_t t = 0; t < tracks.size(); ++t) {
              if (point_of_track[t] >= 0) points[t] = local.points[point_of_track[t]];
            }
          }
        }
      }

      report(progress, Stage::PoseRefinement,
             0.70 + 0.08 * static_cast<double>(attempt + 1) / max_attempts,
             "Registering frames");
    }
  }

  // Final bundle adjustment over everything registered.
  problem.poses = poses;
  problem.points.clear();
  problem.observations.clear();
  for (size_t t = 0; t < tracks.size(); ++t) {
    std::vector<Observation> usable;
    for (const Observation& observation : tracks[t]) {
      if (registered[observation.camera]) usable.push_back(observation);
    }
    if (usable.size() < 2) continue;
    const TriangulationResult tri = triangulateMultiView(poses, intrinsics, usable, 1.0, 8.0);
    if (!tri.valid) continue;
    const int point_index = static_cast<int>(problem.points.size());
    problem.points.push_back(tri.point);
    for (Observation observation : usable) {
      observation.point = point_index;
      problem.observations.push_back(observation);
    }
  }
  problem.constant_poses.assign(poses.size(), 0);
  for (size_t i = 0; i < registered.size(); ++i) {
    // An unregistered frame has no meaningful pose, so hold it fixed rather than letting the
    // solver chase it.
    if (!registered[i]) problem.constant_poses[i] = 1;
  }

  double rmse = 0.0;
  if (problem.valid() && problem.points.size() >= 12) {
    report(progress, Stage::PoseRefinement, 0.78, "Refining camera positions");
    BundleOptions options;
    options.max_iterations = 40;
    // With device poses the scale is metric already; without, pin it so the solution cannot
    // drift in the one direction reprojection error cannot see.
    options.fix_scale = !config.has_device_poses;
    // A sliding window would be needed beyond this, which `max_dense_cameras` enforces.
    options.max_dense_cameras = 400;
    optimizeBundle(&problem, options);
    pruneOutlierObservations(&problem, 4.0);
    if (problem.valid()) {
      options.max_iterations = 25;
      const BundleSummary bundle = optimizeBundle(&problem, options);
      rmse = bundle.final_rmse_px;
    }
    poses = problem.poses;
  }
  report(progress, Stage::PoseRefinement, 0.95, "Saving camera positions");

  int registered_count = 0;
  for (uint8_t flag : registered) registered_count += flag;
  if (registered_count < 2) {
    return StageOutcome{false, false, "fewer than two frames could be registered"};
  }

  Json json = Json::object();
  json.set("scale_factor", Json::number(scale_factor));
  json.set("mean_reprojection_error_px", Json::number(rmse));
  Json list = Json::array();
  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (!registered[i]) continue;
    Json entry = Json::object();
    entry.set("frame", Json::integer(keyframes[i]));
    entry.set("pose", poseToColumnMajor(poses[i]));
    list.push(std::move(entry));
  }
  json.set("keyframes", std::move(list));

  // The triangulated points are persisted too. The depth stage uses them to
  // size its sweep: a fixed 0.25-6 m range over a 30 cm object spends 95% of
  // its samples on empty space, and these points are the only thing that knows
  // how far away the subject actually is.
  Json points = Json::array();
  for (const Vec3& point : problem.points) {
    Json entry = Json::array();
    entry.push(Json::number(point.x()));
    entry.push(Json::number(point.y()));
    entry.push(Json::number(point.z()));
    points.push(std::move(entry));
  }
  json.set("points", std::move(points));

  if (!writeFileText(checkpointPath(Stage::PoseRefinement).string(), json.dump(2))) {
    return StageOutcome{false, false, "could not write the pose checkpoint"};
  }

  summary.registered_frame_count = registered_count;
  summary.mean_reprojection_error_px = rmse;
  summary.scale_factor = scale_factor;
  saveSummary();

  // The counts are in the message, not only the registered total. A run that places three
  // photographs out of eleven goes on to fail two stages later for want of overlap, and
  // "registered 3 frames" was the only place that was visible -- as a success.
  char message[256];
  std::snprintf(message, sizeof(message),
                "registered %d of %zu frames at %.2f px, from %zu matched pairs and %zu tracks",
                registered_count, keyframes.size(), rmse, pairs.size(), tracks.size());

  // Too few placed to reconstruct from, said here rather than discovered downstream. The
  // threshold is a fraction rather than a count because the useful question is whether most of
  // what the user captured made it in, and the mode's own frame budget sets the scale.
  const double placed = keyframes.empty()
                            ? 0.0
                            : static_cast<double>(registered_count) / keyframes.size();
  if (!config.has_device_poses && placed < kMinRegisteredFraction) {
    return StageOutcome{
        false, false,
        std::string(message) +
            "; too few to reconstruct from. The photographs need more overlap: each one should"
            " share about two thirds of its view with the one before it."};
  }
  return StageOutcome{true, false, message};
}

StageOutcome Pipeline::Impl::runDepth(const ProgressCallback& progress,
                                      const CancelToken& cancel) {
  if (!loadFrames()) return StageOutcome{false, false, "frames unavailable"};
  std::vector<int> keyframes;
  std::vector<Pose> poses;
  std::vector<Vec3> sparse_points;
  if (!loadRefinedPoses(&keyframes, &poses, &sparse_points)) {
    return StageOutcome{false, false, "refined poses are missing; re-run pose refinement"};
  }

  // Which keyframes already have a depth map. The capture layer writes sensor
  // depth and the Kotlin ML layer writes monocular network depth into the same
  // directory, so from here the two are indistinguishable by design.
  std::vector<uint8_t> has_depth(keyframes.size(), 0);
  int with_depth = 0;
  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    DepthMap depth;
    if (loadDepth(frames[keyframes[i]], &depth) && !depth.empty()) {
      has_depth[i] = 1;
      ++with_depth;
    }
  }
  report(progress, Stage::Depth, 0.05, "Checking depth maps");

  int estimated = 0;
  if (with_depth < static_cast<int>(keyframes.size())) {
    estimated = estimateMissingDepth(keyframes, poses, sparse_points, has_depth, progress,
                                     cancel);
    if (estimated < 0) return StageOutcome{false, true, "cancelled"};
    with_depth += estimated;
  }

  if (with_depth == 0) {
    return StageOutcome{
        false, false,
        "depth could not be measured or estimated for any frame; the capture may have too "
        "little overlap or too little texture"};
  }

  Json json = Json::object();
  json.set("keyframes_with_depth", Json::integer(with_depth));
  json.set("keyframes_estimated", Json::integer(estimated));
  json.set("keyframes", Json::integer(static_cast<int64_t>(keyframes.size())));
  if (!writeFileText(checkpointPath(Stage::Depth).string(), json.dump(2))) {
    return StageOutcome{false, false, "could not write the depth checkpoint"};
  }

  std::string detail = std::to_string(with_depth) + " of " +
                       std::to_string(keyframes.size()) + " keyframes have depth";
  if (estimated > 0) {
    detail += " (" + std::to_string(estimated) + " estimated from the images)";
  }
  return StageOutcome{true, false, detail};
}

/**
 * Fill in depth for the keyframes that do not have it, by multi-view stereo.
 *
 * This is what lets a phone with no depth sensor -- and no monocular depth
 * model, which is an optional asset -- reconstruct at all. The estimates are
 * written into `depth/` in the same raw float format the capture layer uses, so
 * every later stage is oblivious to where a depth map came from.
 *
 * @return the number of frames estimated, or -1 if cancelled.
 */
int Pipeline::Impl::estimateMissingDepth(const std::vector<int>& keyframes,
                                         const std::vector<Pose>& poses,
                                         const std::vector<Vec3>& sparse_points,
                                         const std::vector<uint8_t>& has_depth,
                                         const ProgressCallback& progress,
                                         const CancelToken& cancel) {
  // Images are loaded once and held as greyscale for the whole stage. A
  // keyframe is a reference for its own estimate and a neighbour for several
  // others, so re-decoding per use would decode each frame five times.
  report(progress, Stage::Depth, 0.1, "Loading frames for depth estimation");
  std::vector<GrayBuffer> grays(keyframes.size());
  std::vector<MvsView> views(keyframes.size());
  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (checkCancel(cancel)) return -1;
    ColorBuffer color;
    if (!loadImage(frames[keyframes[i]], &color) || color.empty()) continue;
    grays[i] = toGray(color);
    views[i].gray = &grays[i];
    views[i].pose = poses[i];
    views[i].intrinsics =
        frames[keyframes[i]].intrinsics.scaledTo(color.width, color.height);
  }

  // A representative scene depth, for choosing neighbours. Taken from the
  // sparse points where they exist, and from the camera path's own extent
  // otherwise -- an orbit's radius is the right order of magnitude for what it
  // is orbiting.
  double scene_depth = 1.0;
  if (!sparse_points.empty()) {
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < views.size(); ++i) {
      if (views[i].gray == nullptr) continue;
      for (const Vec3& point : sparse_points) {
        const Vec3 in_camera = poses[i] * point;
        if (in_camera.z() > 0.0) {
          sum += in_camera.z();
          ++count;
        }
      }
      if (count > 2000) break;
    }
    if (count > 0) scene_depth = sum / count;
  }

  int estimated = 0;
  int attempted = 0;
  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (has_depth[i] || views[i].gray == nullptr) continue;
    if (checkCancel(cancel)) return -1;
    ++attempted;

    const std::vector<int> neighbour_indices =
        selectNeighbours(views, static_cast<int>(i), kMvsNeighbourCount, scene_depth);
    if (neighbour_indices.empty()) continue;
    std::vector<MvsView> neighbours;
    neighbours.reserve(neighbour_indices.size());
    for (int index : neighbour_indices) neighbours.push_back(views[index]);

    MvsConfig mvs;
    mvs.neighbour_count = kMvsNeighbourCount;
    // The sweep's cost is linear in the sample count and quadratic in the
    // resolution, so the quality preset's voxel size -- which is what the user
    // actually chose -- drives both. There is no point estimating depth more
    // finely than the volume can represent.
    const bool coarse = config.voxel_size >= kCoarseVoxelSizeM;
    mvs.downscale = coarse ? kMvsFastDownscale : kMvsDownscale;
    mvs.depth_samples = coarse ? kMvsFastSamples : kMvsSamples;
    if (!depthRangeFromPoints(sparse_points, poses[i], views[i].intrinsics, &mvs.min_depth,
                              &mvs.max_depth)) {
      // No usable sparse points for this view: sweep a range around the scene
      // depth rather than the whole 0.25-6 m default, which would spend most
      // of its samples nowhere near the surface.
      mvs.min_depth = std::max(0.2, scene_depth * 0.35);
      mvs.max_depth = std::max(mvs.min_depth + 0.3, scene_depth * 2.5);
    }
    mvs.max_depth = std::min(mvs.max_depth,
                             static_cast<double>(config.max_integration_depth) * 1.5);

    const DepthMap depth = estimateDepth(views[i], neighbours, mvs);
    if (depth.empty()) continue;

    // Written at the same resolution the estimate produced; the intrinsics in
    // the manifest are at the frame's resolution, and every consumer rescales
    // them to the depth map it is handed.
    const std::string name = depthFileNameFor(keyframes[i]);
    if (writeDepthRaw((projectDir() / "depth" / name).string(), depth)) {
      frames[keyframes[i]].depth_file = name;
      ++estimated;
    }

    const double fraction = 0.15 + 0.8 * (attempted / static_cast<double>(keyframes.size()));
    report(progress, Stage::Depth, std::min(0.95, fraction),
           "Estimating depth " + std::to_string(estimated) + "/" +
               std::to_string(keyframes.size()));
  }
  return estimated;
}

StageOutcome Pipeline::Impl::runFusion(const ProgressCallback& progress,
                                       const CancelToken& cancel) {
  if (!loadFrames()) return StageOutcome{false, false, "frames unavailable"};
  std::vector<int> keyframes;
  std::vector<Pose> poses;
  if (!loadRefinedPoses(&keyframes, &poses)) {
    return StageOutcome{false, false, "refined poses are missing"};
  }

  TsdfConfig tsdf;
  tsdf.voxel_size = config.voxel_size;
  tsdf.truncation = config.voxel_size * config.truncation_multiplier;
  tsdf.integrate_color = true;
  tsdf.max_depth = std::max(0.5f, config.max_integration_depth);
  // Size the block budget from the memory budget rather than a fixed number, so a 4 GB phone
  // degrades by covering less area instead of being killed by the OS.
  const size_t bytes_per_block = 8 * 8 * 8 * 12 + 64;
  tsdf.max_blocks = std::max<size_t>(2048, config.memory_budget_bytes / bytes_per_block);
  TsdfVolume volume(tsdf);

  // Counted so a failure can say which half of the stage had nothing to work with. Without
  // these, "fusion produced nothing" covers both no depth at all and depth that did not line
  // up with the poses, and those have different causes and different things to try.
  size_t frames_with_depth = 0;
  size_t frames_integrated = 0;

  for (size_t i = 0; i < keyframes.size(); ++i) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    const FrameRecord& frame = frames[keyframes[i]];
    DepthMap depth;
    if (!loadDepth(frame, &depth) || depth.empty()) continue;
    ++frames_with_depth;

    ColorBuffer color;
    loadImage(frame, &color);
    // Depth and colour are often different resolutions; the intrinsics must match the depth.
    CameraIntrinsics k = frame.intrinsics;
    if (k.width != depth.width || k.height != depth.height) {
      k = k.scaledTo(depth.width, depth.height);
    }
    if (!color.empty() && (color.width != depth.width || color.height != depth.height)) {
      // Resample colour to the depth resolution so the integrator can pair them per pixel.
      ColorBuffer resized(depth.width, depth.height);
      for (int y = 0; y < depth.height; ++y) {
        const int sy = std::min(color.height - 1, y * color.height / depth.height);
        for (int x = 0; x < depth.width; ++x) {
          const int sx = std::min(color.width - 1, x * color.width / depth.width);
          uint8_t rgb[3];
          color.get(sx, sy, rgb);
          resized.set(x, y, rgb[0], rgb[1], rgb[2]);
        }
      }
      color = std::move(resized);
    }
    volume.integrate(depth, color, k, poses[i]);
    ++frames_integrated;
    report(progress, Stage::Fusion, static_cast<double>(i + 1) / keyframes.size(),
           "Fusing frame " + std::to_string(i + 1) + " of " + std::to_string(keyframes.size()));
  }

  if (keyframes.empty()) {
    return StageOutcome{false, false, "there are no keyframes to fuse"};
  }
  if (frames_with_depth == 0) {
    return StageOutcome{false, false,
                        "none of the " + std::to_string(keyframes.size()) +
                            " keyframes had a depth map, so there was nothing to fuse"};
  }
  if (volume.stats().observed_voxels == 0) {
    return StageOutcome{false, false,
                        "no surface was found in " + std::to_string(frames_integrated) +
                            " of " + std::to_string(keyframes.size()) +
                            " keyframes; the depth maps and camera poses may disagree, or the"
                            " subject may be beyond the " +
                            std::to_string(static_cast<int>(tsdf.max_depth * 100)) +
                            " cm integration range"};
  }
  // Surface the budget exhaustion rather than letting it show up as a mysteriously incomplete
  // model three stages later.
  std::string note;
  if (volume.stats().block_allocation_failures > 0) {
    note = "; the voxel budget was reached, so the far parts of the scene were not fused";
  }
  const std::vector<uint8_t> bytes = volume.serialize();
  if (!writeFileBytes(checkpointPath(Stage::Fusion).string(), bytes)) {
    return StageOutcome{false, false, "could not write the fusion checkpoint"};
  }
  return StageOutcome{true, false,
                      std::to_string(volume.stats().observed_voxels) + " voxels observed" +
                          note};
}

StageOutcome Pipeline::Impl::runMeshing(const ProgressCallback& progress,
                                        const CancelToken& cancel) {
  report(progress, Stage::Meshing, 0.05, "Loading the fused volume");
  std::vector<uint8_t> bytes;
  if (!readFileBytes(checkpointPath(Stage::Fusion).string(), &bytes)) {
    return StageOutcome{false, false, "the fusion checkpoint is missing"};
  }
  TsdfVolume volume;
  if (!volume.deserialize(bytes)) {
    return StageOutcome{false, false, "the fusion checkpoint is corrupt"};
  }
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  report(progress, Stage::Meshing, 0.3, "Extracting the surface");
  Mesh mesh = volume.extractMesh();
  if (mesh.empty()) {
    return StageOutcome{false, false, "no surface was found in the fused volume"};
  }
  report(progress, Stage::Meshing, 0.9, "Saving the mesh");

  ExportOptions options;
  options.binary = true;
  options.include_normals = true;
  if (!writePly(checkpointPath(Stage::Meshing).string(), mesh, options)) {
    return StageOutcome{false, false, "could not write the mesh checkpoint"};
  }
  return StageOutcome{true, false,
                      std::to_string(mesh.triangleCount()) + " triangles extracted"};
}

StageOutcome Pipeline::Impl::runMeshCleanup(const ProgressCallback& progress,
                                            const CancelToken& cancel) {
  Mesh mesh;
  std::string error;
  if (!readPly(checkpointPath(Stage::Meshing).string(), &mesh, &error)) {
    return StageOutcome{false, false, "the mesh checkpoint is missing or unreadable"};
  }
  report(progress, Stage::MeshCleanup, 0.1, "Removing debris");

  // Depth noise always leaves small floating fragments; they are the single most visible
  // artefact in a raw scan.
  removeSmallComponents(&mesh, 64, 0.02);
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  // Auto-level and optionally cut the supporting surface away.
  //
  // Order matters here, and getting it wrong is silent: the table a small object sits on is
  // usually the *largest* connected component in the scan, so running
  // keepLargestComponent before the ground is removed keeps the table and throws the subject
  // away. Ground removal first, then largest-component.
  if (config.auto_level || config.remove_ground_plane) {
    report(progress, Stage::MeshCleanup, 0.3, "Finding the ground plane");
    PlaneDetectionOptions plane_options;
    plane_options.min_inliers = std::max(80, static_cast<int>(mesh.vertexCount() / 60));
    const std::vector<DetectedPlane> planes = detectPlanes(mesh, plane_options);
    if (config.auto_level) {
      const Mat3 correction = levellingRotation(planes, plane_options);
      if (!correction.isApprox(Mat3::Identity(), 1e-9)) {
        transformMesh(&mesh, correction, Vec3::Zero(), 1.0);
      }
      // Persist it even when it is identity, so later stages always find a definite answer
      // rather than guessing whether levelling ran.
      saveLevelRotation(correction);
    }
    if (config.remove_ground_plane) {
      for (const DetectedPlane& plane : planes) {
        if (plane.role != PlaneRole::Floor) continue;
        // Crop away everything at or below the floor, with a small margin so the object's
        // contact patch survives.
        AxisAlignedBox box = meshBounds(mesh);
        const double floor_height = plane.centroid.y();
        box.min.y() = floor_height + config.voxel_size * 1.5;
        cropMesh(&mesh, box);
        removeSmallComponents(&mesh, 32, 0.02);
        break;
      }
    }
  }
  // Now that the supporting surface is gone, the largest component really is the subject.
  if (config.keep_largest_component) keepLargestComponent(&mesh);
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  report(progress, Stage::MeshCleanup, 0.55, "Filling small holes");
  // Only small holes: a large opening is usually the genuinely unseen side of the subject, and
  // closing it would invent geometry the user never captured.
  fillHoles(&mesh, 220);

  // Cap the triangle count at what the texture atlas can actually hold.
  //
  // The per-triangle atlas needs about (atlas / (min_patch + 2 * padding))^2 slots, so geometry
  // beyond that cannot be textured at all. Decimating to fit keeps the mesh and the texture
  // coherent; leaving the extra triangles in would mean shipping geometry with no texture on
  // it, which looks worse than slightly coarser geometry that is fully textured.
  int target = config.target_triangles;
  if (config.texture_resolution > 0) {
    const int slot = 4 + 2 * 1;  // min_patch_texels + minimum gutter on both sides
    const long capacity = static_cast<long>(config.texture_resolution / slot) *
                          (config.texture_resolution / slot);
    // Leave headroom for packing inefficiency.
    const int textureable = static_cast<int>(capacity * 7 / 10);
    target = target > 0 ? std::min(target, textureable) : textureable;
  }
  if (target > 0 && static_cast<int>(mesh.triangleCount()) > target) {
    report(progress, Stage::MeshCleanup, 0.7, "Simplifying");
    // preserve_boundary is deliberately off. A scan's boundary is where the capture ran out of
    // coverage -- a ragged artefact, not a feature -- and pinning it stops the decimator
    // reaching the target at all on an open mesh, which is most of them. (On a watertight mesh
    // there are no boundary edges, so the flag makes no difference either way.)
    decimateMesh(&mesh, target, /*preserve_boundary=*/false);
  }
  recomputeNormals(&mesh);
  if (mesh.empty()) {
    return StageOutcome{false, false, "cleanup removed everything; the scan was too sparse"};
  }

  report(progress, Stage::MeshCleanup, 0.85, "Saving");
  ExportOptions options;
  options.binary = true;
  if (!writePly(checkpointPath(Stage::MeshCleanup).string(), mesh, options)) {
    return StageOutcome{false, false, "could not write the cleaned mesh"};
  }

  // A separate watertight variant for 3D printing; the display mesh keeps its genuine holes.
  if (config.build_watertight) {
    Mesh watertight = mesh;
    fillHoles(&watertight, 100000);
    if (isWatertight(watertight)) {
      writePly((checkpointDir() / "mesh_watertight.ply").string(), watertight, options);
      summary.watertight = true;
    }
  }

  summary.triangle_count = static_cast<long>(mesh.triangleCount());
  summary.vertex_count = static_cast<long>(mesh.vertexCount());
  summary.surface_area_m2 = meshSurfaceArea(mesh);
  summary.volume_m3 = std::abs(meshSignedVolume(mesh));
  summary.bounds = meshBounds(mesh);
  saveSummary();
  return StageOutcome{true, false,
                      std::to_string(mesh.triangleCount()) + " triangles after cleanup"};
}

StageOutcome Pipeline::Impl::runTexturing(const ProgressCallback& progress,
                                          const CancelToken& cancel) {
  Mesh mesh;
  if (!readPly(checkpointPath(Stage::MeshCleanup).string(), &mesh)) {
    return StageOutcome{false, false, "the cleaned mesh is missing"};
  }
  if (!loadFrames()) return StageOutcome{false, false, "frames unavailable"};
  std::vector<int> keyframes;
  std::vector<Pose> poses;
  if (!loadRefinedPoses(&keyframes, &poses)) {
    return StageOutcome{false, false, "refined poses are missing"};
  }
  // The mesh was levelled in cleanup, which rotated the world; bring the cameras with it or
  // every visibility test fails.
  levelPoses(loadLevelRotation(), &poses);

  // Loading every view at once would exceed the memory budget on a long capture, so cap the
  // number used for texturing. Views are sampled evenly over the capture rather than truncated.
  const size_t bytes_per_view = 3ull * 1024 * 1024;
  const size_t view_budget =
      std::max<size_t>(4, std::min<size_t>(keyframes.size(),
                                           config.memory_budget_bytes / 2 / bytes_per_view));
  std::vector<TextureView> views;
  views.reserve(view_budget);
  for (size_t v = 0; v < view_budget; ++v) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    const size_t i = view_budget == 1
                         ? 0
                         : static_cast<size_t>(static_cast<double>(v) * (keyframes.size() - 1) /
                                               (view_budget - 1));
    const FrameRecord& frame = frames[keyframes[i]];
    TextureView view;
    if (!loadImage(frame, &view.image) || view.image.empty()) continue;
    view.intrinsics = frame.intrinsics.width == view.image.width
                          ? frame.intrinsics
                          : frame.intrinsics.scaledTo(view.image.width, view.image.height);
    view.pose = poses[i];
    view.sharpness = std::max(1.0, frame.sharpness);
    view.exposure = frame.exposure;
    DepthMap depth;
    if (loadDepth(frame, &depth) && !depth.empty()) view.depth = std::move(depth);
    views.push_back(std::move(view));
    report(progress, Stage::Texturing, 0.4 * (v + 1) / view_budget, "Loading views");
  }
  if (views.empty()) {
    return StageOutcome{false, false, "no views could be loaded for texturing"};
  }

  report(progress, Stage::Texturing, 0.5, "Selecting views and packing the atlas");
  TexturingOptions options;
  options.atlas_size = config.texture_resolution;
  const TexturingResult result = generateTextureAtlas(mesh, views, options);
  if (!result.success) {
    return StageOutcome{false, false, "texturing failed; no view could see the surface"};
  }
  report(progress, Stage::Texturing, 0.9, "Saving the texture");

  ExportOptions ply_options;
  ply_options.binary = true;
  if (!writePly(checkpointPath(Stage::Texturing).string(), result.mesh, ply_options)) {
    return StageOutcome{false, false, "could not write the textured mesh"};
  }
  if (!writePngFile((checkpointDir() / "atlas.png").string(), result.atlas)) {
    return StageOutcome{false, false, "could not write the texture atlas"};
  }
  summary.has_texture = true;
  saveSummary();
  return StageOutcome{true, false,
                      std::to_string(result.stats.triangles_textured) + " triangles textured"};
}

StageOutcome Pipeline::Impl::runPointCloud(const ProgressCallback& progress,
                                           const CancelToken& cancel) {
  if (!config.build_point_cloud) {
    writeFileText(checkpointPath(Stage::PointCloud).string(), "{\"skipped\":true}");
    return StageOutcome{true, false, "point cloud not requested"};
  }
  report(progress, Stage::PointCloud, 0.1, "Loading the fused volume");
  std::vector<uint8_t> bytes;
  if (!readFileBytes(checkpointPath(Stage::Fusion).string(), &bytes)) {
    return StageOutcome{false, false, "the fusion checkpoint is missing"};
  }
  TsdfVolume volume;
  if (!volume.deserialize(bytes)) {
    return StageOutcome{false, false, "the fusion checkpoint is corrupt"};
  }
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  report(progress, Stage::PointCloud, 0.5, "Extracting points");
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  volume.extractPointCloud(&points, &normals, &colors);
  if (points.empty()) {
    return StageOutcome{false, false, "no surface points were found"};
  }

  // The volume was fused before levelling, so apply the same rotation the mesh received or
  // the cloud and the mesh will not overlay.
  const Mat3 level = loadLevelRotation();
  if (!level.isApprox(Mat3::Identity(), 1e-12)) {
    for (Vec3f& point : points) {
      const Vec3 rotated = level * Vec3(point.x(), point.y(), point.z());
      point = Vec3f(static_cast<float>(rotated.x()), static_cast<float>(rotated.y()),
                    static_cast<float>(rotated.z()));
    }
    for (Vec3f& normal : normals) {
      const Vec3 rotated = (level * Vec3(normal.x(), normal.y(), normal.z())).normalized();
      normal = Vec3f(static_cast<float>(rotated.x()), static_cast<float>(rotated.y()),
                     static_cast<float>(rotated.z()));
    }
  }

  report(progress, Stage::PointCloud, 0.85, "Saving the point cloud");
  ExportOptions options;
  options.binary = true;
  if (!writePointCloudPly(checkpointPath(Stage::PointCloud).string(), points, normals, colors,
                          options)) {
    return StageOutcome{false, false, "could not write the point cloud"};
  }
  summary.point_count = static_cast<long>(points.size());
  saveSummary();
  return StageOutcome{true, false, std::to_string(points.size()) + " points"};
}

StageOutcome Pipeline::Impl::runSplatTraining(const ProgressCallback& progress,
                                              const CancelToken& cancel) {
  if (!config.train_splats) {
    writeFileText(checkpointPath(Stage::SplatTraining).string(), "{\"skipped\":true}");
    return StageOutcome{true, false, "splat training not requested"};
  }

  report(progress, Stage::SplatTraining, 0.02, "Seeding gaussians from the point cloud");
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  if (!readPlyPointCloud(checkpointPath(Stage::PointCloud).string(), &points, &normals, &colors) ||
      points.empty()) {
    writeFileText(checkpointPath(Stage::SplatTraining).string(), "{\"skipped\":true}");
    return StageOutcome{true, false, "no point cloud to seed the splat field from"};
  }

  // Seeding with a quarter of the budget leaves room for densification to
  // find the detail; seeding at the ceiling leaves it nowhere to grow.
  const size_t seed_budget = std::max<size_t>(1, config.splat_max_gaussians / 4);
  if (points.size() > seed_budget) {
    const size_t step = (points.size() + seed_budget - 1) / seed_budget;
    std::vector<Vec3f> kept_points;
    std::vector<std::array<uint8_t, 3>> kept_colors;
    kept_points.reserve(seed_budget);
    kept_colors.reserve(seed_budget);
    for (size_t i = 0; i < points.size(); i += step) {
      kept_points.push_back(points[i]);
      kept_colors.push_back(i < colors.size() ? colors[i] : std::array<uint8_t, 3>{128, 128, 128});
    }
    points.swap(kept_points);
    colors.swap(kept_colors);
  }
  GaussianCloud cloud = initializeFromPointCloud(points, colors, config.splat_sh_degree);
  if (cloud.empty()) {
    return StageOutcome{false, false, "the point cloud produced no gaussians"};
  }

  if (!loadFrames()) return StageOutcome{false, false, "frames unavailable"};
  std::vector<int> keyframes;
  std::vector<Pose> poses;
  if (!loadRefinedPoses(&keyframes, &poses)) {
    return StageOutcome{false, false, "refined poses are missing"};
  }
  // The cloud came from the levelled mesh, so the cameras have to be levelled
  // to match or every projection lands in the wrong place.
  levelPoses(loadLevelRotation(), &poses);

  const size_t view_budget =
      std::min<size_t>(keyframes.size(), static_cast<size_t>(std::max(4, config.splat_max_views)));
  std::vector<SplatTrainingView> views;
  views.reserve(view_budget);
  for (size_t v = 0; v < view_budget; ++v) {
    if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
    const size_t i = view_budget == 1
                         ? 0
                         : static_cast<size_t>(static_cast<double>(v) * (keyframes.size() - 1) /
                                               (view_budget - 1));
    const FrameRecord& frame = frames[keyframes[i]];
    ColorBuffer image;
    if (!loadImage(frame, &image) || image.empty()) continue;
    ColorBuffer scaled = downscaleToWidth(image, config.splat_image_width);

    SplatTrainingView training;
    training.view.intrinsics = frame.intrinsics.scaledTo(scaled.width, scaled.height);
    training.view.camera_from_world = poses[i];
    training.image.resize(static_cast<size_t>(scaled.width) * scaled.height * 3);
    for (size_t p = 0; p < training.image.size(); ++p) {
      training.image[p] = static_cast<float>(scaled.pixels.data()[p]) / 255.f;
    }
    views.push_back(std::move(training));
    report(progress, Stage::SplatTraining, 0.02 + 0.08 * (v + 1) / view_budget,
           "Loading training views");
  }
  if (views.size() < 2) {
    return StageOutcome{false, false, "at least two posed views are needed to train splats"};
  }

  SplatTrainingConfig training_config;
  training_config.iterations = std::max(1, config.splat_iterations);
  training_config.sh_degree = config.splat_sh_degree;
  training_config.max_gaussians = config.splat_max_gaussians;
  training_config.densify_until_iteration = training_config.iterations / 2;
  training_config.opacity_reset_interval = std::max(1, training_config.iterations / 3);
  training_config.sh_increase_interval = std::max(1, training_config.iterations / 8);
  const std::string invalid = training_config.validate();
  if (!invalid.empty()) return StageOutcome{false, false, invalid};

  bool was_cancelled = false;
  bool ran_out_of_time = false;
  int iterations_done = 0;
  const auto training_started = std::chrono::steady_clock::now();
  const SplatTrainingCallback callback = [&](const SplatTrainingProgress& p) {
    if (checkCancel(cancel)) {
      was_cancelled = true;
      return false;
    }
    iterations_done = p.iteration;
    if (config.splat_time_budget_millis > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - training_started)
                               .count();
      if (elapsed >= config.splat_time_budget_millis) {
        // Stop and keep what is trained. Returning false is how training is stopped, and the
        // distinction from a cancellation is this flag: one is the user leaving, the other is
        // the stage deciding it has had its share of the device's evening.
        ran_out_of_time = true;
        return false;
      }
    }
    const double fraction =
        0.1 + 0.85 * static_cast<double>(p.iteration) / std::max(1, p.total_iterations);
    report(progress, Stage::SplatTraining, fraction,
           std::to_string(p.gaussian_count) + " gaussians");
    return true;
  };

  std::string gpu_error;
  if (trainSplatsOnGpu(&cloud, views, training_config, callback, &gpu_error)) {
    summary.splat_backend = "gpu";
  } else {
    if (was_cancelled) return StageOutcome{false, true, "cancelled"};
    if (ran_out_of_time) {
      // The GPU path used the budget up. Nothing to fall back to, and the field it trained is
      // in `cloud` already, so this is the answer rather than the start of a CPU run.
      report(progress, Stage::SplatTraining, 0.97, "Saving the splat field");
      summary.splat_backend = "gpu";
    } else {
    // Falling back rather than failing: a device without a usable compute
    // path still gets a splat field, just a smaller one trained for fewer
    // iterations than a GPU would have managed in the same time.
    SplatTrainingConfig reduced = training_config;
    reduced.iterations = std::max(1, training_config.iterations / 5);
    reduced.max_gaussians = std::min(training_config.max_gaussians, 60000);
    reduced.densify_until_iteration = reduced.iterations / 2;
    reduced.opacity_reset_interval = std::max(1, reduced.iterations / 3);
    reduced.sh_increase_interval = std::max(1, reduced.iterations / 8);
    reduced.sh_degree = std::min(1, config.splat_sh_degree);

    SplatTrainer trainer(std::move(cloud), std::move(views), reduced);
    // A stopped run is not a failed one. `finish()` returns the field as it stands, and a
    // field trained for half its schedule is worth keeping: it is a refinement of a model
    // that already exists, so the alternative to a partial field is no field.
    if (!trainer.train(callback) && was_cancelled) {
      return StageOutcome{false, true, "cancelled"};
    }
    cloud = trainer.finish();
    summary.splat_backend = "cpu";
    }
  }

  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};
  report(progress, Stage::SplatTraining, 0.97, "Saving the splat field");
  if (!writeGaussianPly(checkpointPath(Stage::SplatTraining).string(), cloud)) {
    return StageOutcome{false, false, "could not write the splat field"};
  }
  if (!writeGaussianPly((outputDir() / "splat.ply").string(), cloud)) {
    return StageOutcome{false, false, "could not write splat.ply"};
  }
  summary.splat_count = static_cast<long>(cloud.size());
  saveSummary();
  std::string note = std::to_string(cloud.size()) + " gaussians (" + summary.splat_backend + ")";
  if (ran_out_of_time) {
    note += ", stopped at iteration " + std::to_string(iterations_done) +
            " when the time budget ran out";
  }
  return StageOutcome{true, false, note};
}

StageOutcome Pipeline::Impl::runRoomAnalysis(const ProgressCallback& progress,
                                             const CancelToken& cancel) {
  if (!config.analyse_room) {
    writeFileText(checkpointPath(Stage::RoomAnalysis).string(), "{\"skipped\":true}");
    return StageOutcome{true, false, "room analysis not requested"};
  }
  Mesh mesh;
  if (!readPly(checkpointPath(Stage::MeshCleanup).string(), &mesh)) {
    return StageOutcome{false, false, "the cleaned mesh is missing"};
  }
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  report(progress, Stage::RoomAnalysis, 0.3, "Finding walls, floor and ceiling");
  FloorPlanOptions options;
  options.planes.min_inliers = std::max(120, static_cast<int>(mesh.vertexCount() / 80));
  const FloorPlan plan = generateFloorPlan(mesh, {}, options);

  report(progress, Stage::RoomAnalysis, 0.8, "Drawing the floor plan");
  Json json = Json::object();
  json.set("valid", Json::boolean(plan.valid()));
  json.set("floor_area_m2", Json::number(plan.floor_area));
  json.set("perimeter_m", Json::number(plan.perimeter));
  json.set("floor_height_m", Json::number(plan.floor_height));
  json.set("room_height_m", Json::number(plan.room_height));
  Json outline = Json::array();
  for (const Vec2d& point : plan.outline) {
    Json pair = Json::array();
    pair.push(Json::number(point.x()));
    pair.push(Json::number(point.y()));
    outline.push(std::move(pair));
  }
  json.set("outline", std::move(outline));
  Json openings = Json::array();
  for (const Opening& opening : plan.openings) {
    Json entry = Json::object();
    entry.set("wall", Json::integer(opening.wall_index));
    entry.set("width_m", Json::number(opening.width()));
    entry.set("sill_m", Json::number(opening.sill_height));
    entry.set("head_m", Json::number(opening.head_height));
    entry.set("kind", Json::string(opening.kind == OpeningKind::Window
                                       ? "window"
                                       : (opening.kind == OpeningKind::Door ? "door"
                                                                            : "passage")));
    openings.push(std::move(entry));
  }
  json.set("openings", std::move(openings));
  Json frame = Json::object();
  Json origin = Json::array();
  for (int i = 0; i < 3; ++i) origin.push(Json::number(plan.origin[i]));
  Json x_axis = Json::array();
  for (int i = 0; i < 3; ++i) x_axis.push(Json::number(plan.x_axis[i]));
  Json y_axis = Json::array();
  for (int i = 0; i < 3; ++i) y_axis.push(Json::number(plan.y_axis[i]));
  frame.set("origin", std::move(origin));
  frame.set("x_axis", std::move(x_axis));
  frame.set("y_axis", std::move(y_axis));
  json.set("frame", std::move(frame));

  if (!writeFileText(checkpointPath(Stage::RoomAnalysis).string(), json.dump(2))) {
    return StageOutcome{false, false, "could not write the floor plan"};
  }
  if (plan.valid()) {
    FloorPlanStyle style;
    style.dark = false;
    writeFileText((outputDir() / "floorplan.svg").string(), renderFloorPlanSvg(plan, style));
    writeFileBytes((outputDir() / "floorplan.pdf").string(), renderFloorPlanPdf(plan, style));
    writePngFile((outputDir() / "floorplan.png").string(), renderFloorPlanRaster(plan, style));
    summary.floor_area_m2 = plan.floor_area;
    summary.ceiling_height_m = plan.room_height;
    summary.has_floor_plan = true;
    saveSummary();
  }
  return StageOutcome{true, false,
                      plan.valid() ? "floor plan measured" : "no floor plan could be measured"};
}

StageOutcome Pipeline::Impl::runFinalise(const ProgressCallback& progress,
                                         const CancelToken& cancel) {
  report(progress, Stage::Finalise, 0.2, "Writing the shareable model");
  // The display mesh is the textured one when texturing succeeded.
  Mesh mesh;
  const bool textured = readPly(checkpointPath(Stage::Texturing).string(), &mesh);
  if (!textured && !readPly(checkpointPath(Stage::MeshCleanup).string(), &mesh)) {
    return StageOutcome{false, false, "no mesh to finalise"};
  }
  if (checkCancel(cancel)) return StageOutcome{false, true, "cancelled"};

  ColorBuffer atlas;
  if (textured) {
    std::vector<uint8_t> png;
    if (readFileBytes((checkpointDir() / "atlas.png").string(), &png)) {
      decodePng(png, &atlas);
    }
  }

  // GLB is written eagerly because it is both the default export and what the in-app viewer
  // loads; every other format is produced on demand.
  ExportOptions options;
  options.name = "ParallaxModel";
  options.max_texture_size = config.texture_resolution;
  if (!writeGlb((outputDir() / "model.glb").string(), mesh, atlas, options)) {
    return StageOutcome{false, false, "could not write model.glb"};
  }
  report(progress, Stage::Finalise, 0.8, "Recording measurements");

  summary.triangle_count = static_cast<long>(mesh.triangleCount());
  summary.vertex_count = static_cast<long>(mesh.vertexCount());
  summary.surface_area_m2 = meshSurfaceArea(mesh);
  summary.volume_m3 = std::abs(meshSignedVolume(mesh));
  summary.bounds = meshBounds(mesh);
  summary.has_texture = textured && !atlas.empty();
  saveSummary();

  Json json = Json::object();
  json.set("complete", Json::boolean(true));
  json.set("textured", Json::boolean(summary.has_texture));
  if (!writeFileText(checkpointPath(Stage::Finalise).string(), json.dump(2))) {
    return StageOutcome{false, false, "could not write the finalise checkpoint"};
  }
  return StageOutcome{true, false, "model ready"};
}

// ------------------------------------------------------------------- facade

Pipeline::Pipeline(PipelineConfig config) : impl_(new Impl(std::move(config))) {}
Pipeline::~Pipeline() = default;

const PipelineSummary& Pipeline::summary() const { return impl_->summary; }
const PipelineConfig& Pipeline::config() const { return impl_->config; }

Stage Pipeline::nextStage() const {
  for (int s = 0; s < static_cast<int>(Stage::Count); ++s) {
    const Stage stage = static_cast<Stage>(s);
    if (!impl_->hasCheckpoint(stage)) return stage;
  }
  return Stage::Count;
}

bool Pipeline::isComplete() const { return nextStage() == Stage::Count; }

void Pipeline::invalidateFrom(Stage stage) {
  std::error_code ec;
  for (int s = static_cast<int>(stage); s < static_cast<int>(Stage::Count); ++s) {
    fs::remove(impl_->checkpointPath(static_cast<Stage>(s)), ec);
  }
}

StageOutcome Pipeline::runStage(Stage stage, const ProgressCallback& progress,
                                const CancelToken& cancel) {
  switch (stage) {
    case Stage::Ingest: return impl_->runIngest(progress, cancel);
    case Stage::PoseRefinement: return impl_->runPoseRefinement(progress, cancel);
    case Stage::Depth: return impl_->runDepth(progress, cancel);
    case Stage::Fusion: return impl_->runFusion(progress, cancel);
    case Stage::Meshing: return impl_->runMeshing(progress, cancel);
    case Stage::MeshCleanup: return impl_->runMeshCleanup(progress, cancel);
    case Stage::Texturing: return impl_->runTexturing(progress, cancel);
    case Stage::PointCloud: return impl_->runPointCloud(progress, cancel);
    case Stage::SplatTraining: return impl_->runSplatTraining(progress, cancel);
    case Stage::RoomAnalysis: return impl_->runRoomAnalysis(progress, cancel);
    case Stage::Finalise: return impl_->runFinalise(progress, cancel);
    default: return StageOutcome{false, false, "unknown stage"};
  }
}

StageOutcome Pipeline::runToCompletion(const ProgressCallback& progress,
                                      const CancelToken& cancel) {
  StageOutcome outcome{true, false, "already complete"};
  while (true) {
    const Stage stage = nextStage();
    if (stage == Stage::Count) break;
    outcome = runStage(stage, progress, cancel);
    if (!outcome.success) return outcome;
  }
  return outcome;
}

Mesh Pipeline::loadMesh() const {
  Mesh mesh;
  if (readPly(impl_->checkpointPath(Stage::Texturing).string(), &mesh)) return mesh;
  if (readPly(impl_->checkpointPath(Stage::MeshCleanup).string(), &mesh)) return mesh;
  if (readPly(impl_->checkpointPath(Stage::Meshing).string(), &mesh)) return mesh;
  return Mesh{};
}

bool Pipeline::loadPointCloud(std::vector<Vec3f>* points, std::vector<Vec3f>* normals,
                              std::vector<std::array<uint8_t, 3>>* colors) const {
  return readPlyPointCloud(impl_->checkpointPath(Stage::PointCloud).string(), points, normals,
                           colors);
}

ColorBuffer Pipeline::loadAtlas() const {
  std::vector<uint8_t> png;
  ColorBuffer atlas;
  if (readFileBytes((impl_->checkpointDir() / "atlas.png").string(), &png)) {
    decodePng(png, &atlas);
  }
  return atlas;
}

FloorPlan Pipeline::loadFloorPlan() const {
  FloorPlan plan;
  std::string text;
  if (!readFileText(impl_->checkpointPath(Stage::RoomAnalysis).string(), &text)) return plan;
  Json json;
  if (!Json::parse(text, &json)) return plan;
  if (!json.get("valid").asBool()) return plan;

  plan.floor_area = json.get("floor_area_m2").asDouble();
  plan.perimeter = json.get("perimeter_m").asDouble();
  plan.floor_height = json.get("floor_height_m").asDouble();
  plan.room_height = json.get("room_height_m").asDouble();
  const Json& outline = json.get("outline");
  for (size_t i = 0; i < outline.size(); ++i) {
    const Json& pair = outline.at(i);
    if (pair.size() != 2) continue;
    plan.outline.emplace_back(pair.at(0).asDouble(), pair.at(1).asDouble());
  }
  const Json& frame = json.get("frame");
  const auto readVec = [](const Json& array, Vec3 fallback) {
    if (!array.isArray() || array.size() != 3) return fallback;
    return Vec3(array.at(0).asDouble(), array.at(1).asDouble(), array.at(2).asDouble());
  };
  plan.origin = readVec(frame.get("origin"), Vec3::Zero());
  plan.x_axis = readVec(frame.get("x_axis"), Vec3::UnitX());
  plan.y_axis = readVec(frame.get("y_axis"), Vec3::UnitZ());

  // Rebuild the wall segments from the outline so callers get a complete plan.
  for (size_t i = 0; i < plan.outline.size(); ++i) {
    WallSegment wall;
    wall.start = plan.outline[i];
    wall.end = plan.outline[(i + 1) % plan.outline.size()];
    const Vec2d edge = wall.end - wall.start;
    if (edge.norm() < 1e-9) continue;
    wall.normal = Vec2d(edge.y(), -edge.x()).normalized();
    wall.height = plan.room_height;
    plan.walls.push_back(wall);
  }
  return plan;
}

}  // namespace parallax
