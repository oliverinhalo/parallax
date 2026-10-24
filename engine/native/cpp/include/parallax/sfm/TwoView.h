// Two-view geometry: fundamental/essential matrices, pose recovery, homography.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/sfm/Ransac.h"

namespace parallax {

/// A pixel correspondence between two images.
struct PointPair {
  Vec2 a = Vec2::Zero();
  Vec2 b = Vec2::Zero();
};

/**
 * Hartley normalisation: translate points to zero mean and scale so the mean
 * distance from the origin is sqrt(2). Without this the 8-point algorithm is
 * numerically hopeless, because pixel coordinates in the hundreds make the
 * constraint matrix's columns differ in magnitude by ~10^5.
 *
 * Returns the transform T such that normalised = T * homogeneous(point).
 */
Mat3 normalizingTransform(const std::vector<Vec2>& points);

/// Fundamental matrix from >= 8 correspondences (normalised 8-point algorithm,
/// with the rank-2 constraint enforced by zeroing the smallest singular value).
bool fundamentalFromPairs(const std::vector<PointPair>& pairs, Mat3* out_F);

/// Essential matrix from a fundamental matrix and both cameras' intrinsics.
Mat3 essentialFromFundamental(const Mat3& F, const CameraIntrinsics& ka,
                              const CameraIntrinsics& kb);

/// Essential matrix directly from normalised (calibrated) correspondences.
bool essentialFromNormalizedPairs(const std::vector<PointPair>& normalized_pairs, Mat3* out_E);

/// Project E onto the essential manifold (singular values {s, s, 0}).
Mat3 projectToEssential(const Mat3& E);

/// The four (R, t) candidates consistent with an essential matrix. `t` is
/// recovered up to sign and scale, so it is returned unit length.
void decomposeEssential(const Mat3& E, std::array<Pose, 4>* out_candidates);

/**
 * Pick the (R, t) candidate with the most points in front of both cameras.
 *
 * @param pairs  pixel correspondences
 * @param ka,kb  intrinsics of the two views
 * @param out_inliers  per-pair cheirality mask (may be null)
 * @return the relative pose b-from-a, with unit baseline
 */
bool recoverPoseFromEssential(const Mat3& E, const std::vector<PointPair>& pairs,
                              const CameraIntrinsics& ka, const CameraIntrinsics& kb,
                              Pose* out_pose, std::vector<uint8_t>* out_inliers);

/// Sampson distance of a correspondence under F, in pixels (first-order
/// geometric error; strictly better behaved than the algebraic residual).
double sampsonDistance(const Mat3& F, const PointPair& pair);

/// Symmetric epipolar distance in pixels.
double symmetricEpipolarDistance(const Mat3& F, const PointPair& pair);

/// Homography from >= 4 correspondences (normalised DLT).
bool homographyFromPairs(const std::vector<PointPair>& pairs, Mat3* out_H);

/// Symmetric transfer error of a correspondence under H, in pixels.
double homographyTransferError(const Mat3& H, const PointPair& pair);

struct TwoViewGeometry {
  Pose relative_pose;             ///< b-from-a, unit baseline
  Mat3 essential = Mat3::Zero();
  Mat3 fundamental = Mat3::Zero();
  std::vector<uint8_t> inliers;
  int inlier_count = 0;
  double median_sampson = 0.0;
  /// Fraction of inliers also explained by a homography. Close to 1 means the
  /// scene is planar (or the motion is pure rotation) and the essential matrix
  /// is degenerate, so the pair must not be used to initialise the map.
  double planarity = 0.0;
  bool success = false;
};

/// Robust two-view estimation: RANSAC over the essential matrix, pose recovery,
/// and a planarity check that rejects degenerate initialisation pairs.
TwoViewGeometry estimateTwoView(const std::vector<PointPair>& pairs,
                                const CameraIntrinsics& ka, const CameraIntrinsics& kb,
                                const RansacOptions& options = {});

}  // namespace parallax
