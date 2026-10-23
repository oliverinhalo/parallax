// Sparse bundle adjustment over camera poses and 3D points.
//
// Implemented directly on Eigen rather than delegating to Ceres. The structure
// of the problem is fixed and well understood -- 6-DoF poses, 3-DoF points, a
// 2-D reprojection residual -- so a purpose-built Levenberg-Marquardt solver
// with an explicit Schur complement is a few hundred lines, has no external
// dependency to cross-compile for Android, and is fully host-testable. See
// docs/DECISIONS.md (ADR-004) for the trade-off.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/sfm/Triangulation.h"

#include <string>

namespace parallax {

/// The optimisation problem. Poses and points are updated in place.
struct BundleProblem {
  /// Camera-from-world poses, one per keyframe.
  std::vector<Pose> poses;
  /// Intrinsics per keyframe. Treated as constant: phone cameras report
  /// reliable intrinsics, and letting them float on short handheld sequences
  /// trades a little reprojection error for a lot of drift.
  std::vector<CameraIntrinsics> intrinsics;
  /// World-space landmarks.
  std::vector<Vec3> points;
  /// Observations linking a pose to a point at a pixel.
  std::vector<Observation> observations;

  /// Optional per-element masks; 1 means "hold fixed". Empty means all free.
  std::vector<uint8_t> constant_poses;
  std::vector<uint8_t> constant_points;

  bool poseIsConstant(int i) const {
    return i < static_cast<int>(constant_poses.size()) && constant_poses[i] != 0;
  }
  bool pointIsConstant(int i) const {
    return i < static_cast<int>(constant_points.size()) && constant_points[i] != 0;
  }
  bool valid() const;
};

struct BundleOptions {
  int max_iterations = 60;
  /// Relative decrease in cost below which we declare convergence.
  double function_tolerance = 1e-8;
  double gradient_tolerance = 1e-12;
  double parameter_tolerance = 1e-10;
  /// Huber threshold in pixels. Residuals beyond this are down-weighted rather
  /// than discarded, so a mismatch degrades gracefully instead of pulling the
  /// solution apart.
  double huber_delta_px = 2.5;
  double initial_lambda = 1e-4;
  double min_lambda = 1e-12;
  double max_lambda = 1e12;
  /**
   * Fix the first non-constant pose. Reprojection error is invariant to a
   * global rigid transform, so without a gauge constraint the normal equations
   * are singular in six directions.
   */
  bool fix_first_pose = true;
  /**
   * Also preserve the baseline between the first two free cameras. Monocular
   * reconstruction is scale-invariant, giving a seventh null direction; enable
   * this when there is no metric prior (pure photo/video import) and leave it
   * off when ARCore has supplied true scale.
   */
  bool fix_scale = false;
  /// Guard against the dense reduced camera system exploding in memory.
  int max_dense_cameras = 600;
  bool verbose = false;
};

struct BundleSummary {
  double initial_cost = 0.0;
  double final_cost = 0.0;
  double initial_rmse_px = 0.0;
  double final_rmse_px = 0.0;
  int iterations = 0;
  int successful_steps = 0;
  int observations_used = 0;
  bool converged = false;
  bool success = false;
  std::string message;
};

/// Run Levenberg-Marquardt. `problem` is modified in place.
BundleSummary optimizeBundle(BundleProblem* problem, const BundleOptions& options = {});

/// Root-mean-square reprojection error in pixels over all usable observations.
double bundleRmsePixels(const BundleProblem& problem, int* out_used = nullptr);

/// Drop observations whose reprojection error exceeds `threshold_px`, and then
/// any point left with fewer than two observations. Returns the number removed.
int pruneOutlierObservations(BundleProblem* problem, double threshold_px);

}  // namespace parallax
