// The reconstruction pipeline: a staged, resumable, cancellable orchestrator.
//
// Every stage reads its inputs from the project directory and writes its outputs back there as
// a checkpoint. That is what makes the pipeline survive process death: Android will kill a
// background service mid-scan, and a twenty-minute reconstruction that restarts from zero is
// not a feature anyone will use twice. On resume the pipeline inspects the checkpoints already
// on disk and continues from the first stage that has not completed.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/floorplan/FloorPlan.h"
#include "parallax/fusion/TsdfVolume.h"
#include "parallax/geometry/SE3.h"
#include "parallax/mesh/Mesh.h"
#include "parallax/splat/GaussianCloud.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace parallax {

enum class CaptureMode { Object, Room, Area, Import };

/// Stage identifiers. The ordering is the execution order and must match
/// `ReconstructionStage` on the Kotlin side.
enum class Stage {
  Ingest = 0,
  PoseRefinement = 1,
  Depth = 2,
  Fusion = 3,
  Meshing = 4,
  MeshCleanup = 5,
  Texturing = 6,
  PointCloud = 7,
  SplatTraining = 8,
  RoomAnalysis = 9,
  Finalise = 10,
  Count = 11,
};

const char* stageName(Stage stage);
/// Fraction of total runtime this stage accounts for, measured on a mid-range device.
double stageWeight(Stage stage);

struct PipelineConfig {
  /// Directory holding `manifest.json`, `frames/`, `depth/`, and where checkpoints and outputs
  /// are written.
  std::string project_dir;
  CaptureMode mode = CaptureMode::Object;

  float voxel_size = 0.008f;
  float truncation_multiplier = 4.0f;
  int max_keyframes = 160;
  int texture_resolution = 4096;
  int target_triangles = 300000;

  /// Set when the capture supplied device poses; when false the pipeline runs full
  /// structure-from-motion in the pose stage.
  bool has_device_poses = true;
  /// Set when per-frame depth maps are present in `depth/`.
  bool has_depth_maps = true;

  /// Memory budget for the fusion volume, from the device's RAM.
  size_t memory_budget_bytes = 512ull * 1024 * 1024;

  /**
   * Furthest depth to integrate, in metres.
   *
   * This is a quality setting, not just a performance one. The voxel block budget is finite,
   * and integrating the floor stretching away behind a small object spends the whole budget on
   * surface nobody wants -- leaving none for the subject, so the extraction can come back
   * empty. Object captures therefore cut off close in; room and area captures need the range.
   */
  float max_integration_depth = 3.0f;

  bool remove_ground_plane = false;   ///< object mode: cut the supporting surface away
  bool keep_largest_component = false; ///< object mode: discard floating debris
  bool auto_level = true;              ///< rotate so the detected floor is horizontal
  bool build_point_cloud = true;
  bool build_watertight = true;
  bool analyse_room = false;

  /**
   * Gaussian splatting.
   *
   * Off by default: it is the longest stage by a wide margin and the mesh is
   * what most captures are actually for. `splat_image_width` is the width the
   * training views are downscaled to -- the splat field resolves detail from
   * having many views rather than large ones, and full-resolution frames cost
   * memory the optimiser needs for gaussians.
   */
  bool train_splats = false;
  int splat_iterations = 5000;
  int splat_max_gaussians = 180000;
  int splat_sh_degree = 2;
  int splat_image_width = 512;
  int splat_max_views = 80;
};

/// Reported as each stage advances.
struct Progress {
  Stage stage = Stage::Ingest;
  double stage_fraction = 0.0;
  double overall_fraction = 0.0;
  std::string detail;
};

using ProgressCallback = std::function<void(const Progress&)>;

/// Checked between units of work; set it to abort cooperatively.
using CancelToken = std::shared_ptr<std::atomic_bool>;

struct StageOutcome {
  bool success = false;
  bool cancelled = false;
  std::string message;
};

/// Summary of the finished reconstruction, read by the app for the library and viewer.
struct PipelineSummary {
  int frame_count = 0;
  int keyframe_count = 0;
  int registered_frame_count = 0;
  double mean_reprojection_error_px = 0.0;
  long triangle_count = 0;
  long vertex_count = 0;
  long point_count = 0;
  long splat_count = 0;
  double surface_area_m2 = 0.0;
  double volume_m3 = 0.0;
  bool watertight = false;
  AxisAlignedBox bounds;
  double floor_area_m2 = 0.0;
  double ceiling_height_m = 0.0;
  bool has_floor_plan = false;
  bool has_texture = false;
  double scale_factor = 1.0;
  /// Which backend trained the splats, for the processing log: "gpu", "cpu"
  /// or empty when the stage did not run.
  std::string splat_backend;
};

class Pipeline {
 public:
  explicit Pipeline(PipelineConfig config);
  ~Pipeline();
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  /// The first stage whose checkpoint is absent, i.e. where a resumed run should start.
  Stage nextStage() const;

  /// True when every stage has a checkpoint on disk.
  bool isComplete() const;

  /// Run exactly one stage. Safe to call again after a failure or cancellation.
  StageOutcome runStage(Stage stage, const ProgressCallback& progress, const CancelToken& cancel);

  /// Run from [nextStage] to the end, stopping on failure or cancellation.
  StageOutcome runToCompletion(const ProgressCallback& progress, const CancelToken& cancel);

  /// Discard checkpoints from `stage` onwards, so it and everything after it re-runs.
  void invalidateFrom(Stage stage);

  const PipelineSummary& summary() const;
  const PipelineConfig& config() const;

  /// Load the finished mesh (textured if available). Empty if meshing has not completed.
  Mesh loadMesh() const;
  /// Load the point cloud. Empty if the point cloud stage has not completed.
  bool loadPointCloud(std::vector<Vec3f>* points, std::vector<Vec3f>* normals,
                      std::vector<std::array<uint8_t, 3>>* colors) const;
  /// Load the texture atlas. Empty if texturing has not completed.
  ColorBuffer loadAtlas() const;
  /// The computed floor plan. Invalid if room analysis has not completed.
  FloorPlan loadFloorPlan() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace parallax
