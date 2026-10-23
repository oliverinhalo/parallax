// Absolute pose estimation: recovering a camera pose from 2D-3D correspondences.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/sfm/Ransac.h"

namespace parallax {

/// A 2D observation of a known 3D point.
struct PointCorrespondence {
  Vec3 world = Vec3::Zero();
  Vec2 pixel = Vec2::Zero();
};

/**
 * Linear DLT absolute pose from >= 6 correspondences.
 *
 * Solves for the 3x4 projection matrix directly, then projects its left 3x3
 * block back onto SO(3). The 3D points are pre-conditioned by a similarity
 * transform (zero mean, unit RMS radius); without it the constraint matrix is
 * badly scaled whenever the scene is far from the origin, which is the normal
 * case once a reconstruction has drifted a few metres.
 */
bool pnpDlt(const std::vector<PointCorrespondence>& correspondences, const CameraIntrinsics& k,
            Pose* out_pose);

struct PnpResult {
  Pose pose;
  std::vector<uint8_t> inliers;
  int inlier_count = 0;
  double rmse_px = 0.0;
  bool success = false;
};

/// RANSAC over `pnpDlt`, followed by a Gauss-Newton refinement on the inliers.
PnpResult pnpRansac(const std::vector<PointCorrespondence>& correspondences,
                    const CameraIntrinsics& k, const RansacOptions& options = {});

/**
 * Motion-only bundle adjustment: refine a single pose against fixed 3D points.
 *
 * Uses Gauss-Newton on the SE(3) manifold with a Huber loss, so a handful of
 * surviving mismatches cannot drag the pose away. Returns the RMSE in pixels
 * over the points whose weight stayed at 1.
 */
bool refinePoseGaussNewton(const std::vector<PointCorrespondence>& correspondences,
                           const CameraIntrinsics& k, Pose* pose, int max_iterations = 20,
                           double huber_delta_px = 2.5, double* out_rmse_px = nullptr);

/**
 * Jacobian of the pixel projection with respect to a left-multiplied SE(3)
 * increment, evaluated at `pose`. Exposed because bundle adjustment and the
 * pose refiner must agree exactly, and because it is worth testing against
 * finite differences on its own.
 *
 * Tangent ordering is [translation(3), rotation(3)].
 */
bool projectionJacobian(const Pose& pose, const CameraIntrinsics& k, const Vec3& world_point,
                        Vec2* out_pixel, Eigen::Matrix<double, 2, 6>* out_J_pose,
                        Eigen::Matrix<double, 2, 3>* out_J_point);

}  // namespace parallax
