// Multi-view stereo: depth from images and known poses.
//
// This is the fallback that makes reconstruction possible at all on a device
// with no depth sensor and no monocular depth model. Sensor depth is better
// where it exists and a learned monocular model is better on untextured
// surfaces, but neither is always available, and a scanner that cannot
// reconstruct without either is not a scanner.
//
// The method is plane-sweep with normalised cross-correlation against a handful
// of neighbouring views, winner-takes-all, then a geometric consistency check
// that discards depths the neighbours disagree with. That last step is what
// makes the output usable for fusion: a confident wrong depth is far more
// damaging than a missing one, because TSDF integration will carve a hole
// through the surface for it.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"

#include <vector>

namespace parallax {

struct MvsConfig {
  /// Depth hypotheses per pixel. Cost is linear in this.
  int depth_samples = 48;

  /// Search range, in metres. Clamped to what the sparse points suggest when
  /// those are supplied.
  double min_depth = 0.25;
  double max_depth = 6.0;

  /// Half-width of the NCC window. 2 gives a 5x5 patch, which is the smallest
  /// that is stable on real texture.
  int window_radius = 2;

  /// Neighbouring views matched against the reference. More is more robust and
  /// linearly more expensive; four is where the returns flatten.
  int neighbour_count = 4;

  /// Minimum NCC to accept a match. Below this the pixel is left unmeasured.
  double min_correlation = 0.5;

  /// A depth survives if at least this many *other* views, reprojected, agree
  /// with it to within `consistency_tolerance`.
  int min_consistent_views = 2;

  /// Relative depth agreement required, as a fraction of the depth itself.
  double consistency_tolerance = 0.012;

  /// Downsample factor for the estimate. Depth is smooth, fusion voxelises it
  /// anyway, and the cost is quadratic in resolution, so half resolution is
  /// the default.
  int downscale = 2;
  /// Worker threads for the sweep; zero asks the hardware, capped internally.
  int threads = 0;
};

/// One view's contribution: the image, its pose and its intrinsics.
struct MvsView {
  const GrayBuffer* gray = nullptr;
  Pose pose;  ///< world-from-camera
  CameraIntrinsics intrinsics;
};

/**
 * Estimate depth for `reference`, matching against `neighbours`.
 *
 * Returns an empty map when there is not enough baseline to say anything, which
 * is the honest answer for a sequence captured from one position.
 */
DepthMap estimateDepth(const MvsView& reference, const std::vector<MvsView>& neighbours,
                       const MvsConfig& config = MvsConfig());

/**
 * Choose which views to match against a reference.
 *
 * Views too close to the reference have no parallax and views too far have no
 * overlap, so the score rewards a baseline-to-depth ratio in a usable band and
 * penalises large viewing-angle differences.
 */
std::vector<int> selectNeighbours(const std::vector<MvsView>& views, int reference_index,
                                  int count, double scene_depth);

/**
 * A depth range from sparse points, for sizing the sweep.
 *
 * Sweeping a fixed 0.25-6 m range over a 30 cm object spends 95% of the samples
 * on empty space; deriving it from the structure-from-motion points that are
 * already available makes every sample count.
 */
bool depthRangeFromPoints(const std::vector<Vec3>& world_points, const Pose& pose,
                          const CameraIntrinsics& intrinsics, double* out_min,
                          double* out_max);

}  // namespace parallax
