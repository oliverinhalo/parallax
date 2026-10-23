#include "parallax/sfm/Triangulation.h"

#include <Eigen/SVD>
#include <cmath>
#include <limits>

namespace parallax {
namespace {

/// Append the two DLT rows contributed by one observation.
///
/// For a camera with projection P = K [R|t] and an observation x = (u, v), the
/// constraint x_homogeneous ~ P X gives (after eliminating the scale):
///     u * P_row2 - P_row0 = 0
///     v * P_row2 - P_row1 = 0
/// We work with calibrated rays instead of pixels so the two rows are already
/// balanced in magnitude, which removes the need for a separate normalisation.
void appendDltRows(const Pose& pose, const CameraIntrinsics& k, const Vec2& px,
                   Eigen::MatrixXd* A, int row) {
  const Vec3 ray = k.unproject(px);  // (x, y, 1) in camera coordinates
  const Mat34 P = pose.matrix34();
  A->row(row) = ray.x() * P.row(2) - P.row(0);
  A->row(row + 1) = ray.y() * P.row(2) - P.row(1);
}

TriangulationResult solveAndValidate(const Eigen::MatrixXd& A, const std::vector<Pose>& poses,
                                     const std::vector<CameraIntrinsics>& intrinsics,
                                     const std::vector<Vec2>& pixels, double min_parallax_deg,
                                     double max_reprojection_px) {
  TriangulationResult result;
  // The solution is the null vector of A, i.e. the right singular vector with
  // the smallest singular value.
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
  const Vec4 h = svd.matrixV().col(3);
  if (std::abs(h.w()) < 1e-12) {
    // Point at infinity: pure rotation, or zero parallax.
    return result;
  }
  result.point = h.head<3>() / h.w();

  // Cheirality: the point must be in front of every camera that saw it.
  for (size_t i = 0; i < poses.size(); ++i) {
    if ((poses[i] * result.point).z() <= 1e-6) return result;
  }

  // Parallax: take the widest angle over all camera pairs. One well-separated
  // pair is enough to pin the depth down even if the rest are close together.
  double best_parallax = 0.0;
  for (size_t i = 0; i < poses.size(); ++i) {
    for (size_t j = i + 1; j < poses.size(); ++j) {
      best_parallax = std::max(
          best_parallax,
          parallaxDegrees(poses[i].center(), poses[j].center(), result.point));
    }
  }
  result.parallax_deg = best_parallax;
  if (best_parallax < min_parallax_deg) return result;

  for (size_t i = 0; i < poses.size(); ++i) {
    const double err = reprojectionError(poses[i], intrinsics[i], result.point, pixels[i]);
    result.max_reprojection_px = std::max(result.max_reprojection_px, err);
  }
  if (!(result.max_reprojection_px <= max_reprojection_px)) return result;

  result.valid = true;
  return result;
}

}  // namespace

double parallaxDegrees(const Vec3& center_a, const Vec3& center_b, const Vec3& point) {
  const Vec3 ra = point - center_a;
  const Vec3 rb = point - center_b;
  const double na = ra.norm(), nb = rb.norm();
  if (na < 1e-12 || nb < 1e-12) return 0.0;
  const double c = std::max(-1.0, std::min(1.0, ra.dot(rb) / (na * nb)));
  return rad2deg(std::acos(c));
}

double reprojectionError(const Pose& pose, const CameraIntrinsics& k, const Vec3& point,
                         const Vec2& observed) {
  Vec2 projected;
  if (!k.project(pose * point, &projected)) return std::numeric_limits<double>::infinity();
  return (projected - observed).norm();
}

TriangulationResult triangulateDlt(const Pose& pose_a, const CameraIntrinsics& ka,
                                   const Vec2& px_a, const Pose& pose_b,
                                   const CameraIntrinsics& kb, const Vec2& px_b,
                                   double min_parallax_deg, double max_reprojection_px) {
  Eigen::MatrixXd A(4, 4);
  appendDltRows(pose_a, ka, px_a, &A, 0);
  appendDltRows(pose_b, kb, px_b, &A, 2);
  return solveAndValidate(A, {pose_a, pose_b}, {ka, kb}, {px_a, px_b}, min_parallax_deg,
                          max_reprojection_px);
}

TriangulationResult triangulateMultiView(const std::vector<Pose>& poses,
                                         const std::vector<CameraIntrinsics>& intrinsics,
                                         const std::vector<Observation>& observations,
                                         double min_parallax_deg, double max_reprojection_px) {
  TriangulationResult result;
  if (observations.size() < 2) return result;

  std::vector<Pose> used_poses;
  std::vector<CameraIntrinsics> used_intrinsics;
  std::vector<Vec2> used_pixels;
  used_poses.reserve(observations.size());
  used_intrinsics.reserve(observations.size());
  used_pixels.reserve(observations.size());

  for (const Observation& obs : observations) {
    if (obs.camera < 0 || obs.camera >= static_cast<int>(poses.size())) return result;
    if (obs.camera >= static_cast<int>(intrinsics.size())) return result;
    used_poses.push_back(poses[obs.camera]);
    used_intrinsics.push_back(intrinsics[obs.camera]);
    used_pixels.push_back(obs.pixel);
  }

  Eigen::MatrixXd A(static_cast<int>(used_poses.size()) * 2, 4);
  for (size_t i = 0; i < used_poses.size(); ++i) {
    appendDltRows(used_poses[i], used_intrinsics[i], used_pixels[i], &A,
                  static_cast<int>(i) * 2);
  }
  return solveAndValidate(A, used_poses, used_intrinsics, used_pixels, min_parallax_deg,
                          max_reprojection_px);
}

}  // namespace parallax
