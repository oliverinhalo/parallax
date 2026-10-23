#include "parallax/sfm/Pnp.h"

#include <Eigen/SVD>
#include <cmath>

namespace parallax {
namespace {

/// Similarity transform that centres a point set and scales it to unit RMS radius.
struct PointNormalizer {
  Vec3 centroid = Vec3::Zero();
  double scale = 1.0;

  static PointNormalizer build(const std::vector<PointCorrespondence>& c,
                               const std::vector<int>& indices) {
    PointNormalizer n;
    if (indices.empty()) return n;
    for (int i : indices) n.centroid += c[i].world;
    n.centroid /= static_cast<double>(indices.size());
    double rms = 0.0;
    for (int i : indices) rms += (c[i].world - n.centroid).squaredNorm();
    rms = std::sqrt(rms / static_cast<double>(indices.size()));
    n.scale = rms > 1e-9 ? std::sqrt(3.0) / rms : 1.0;
    return n;
  }

  Vec3 apply(const Vec3& p) const { return (p - centroid) * scale; }

  /// The 4x4 transform T with normalized = T * homogeneous(p).
  Mat4 matrix() const {
    Mat4 T = Mat4::Identity();
    T.topLeftCorner<3, 3>() = Mat3::Identity() * scale;
    T.topRightCorner<3, 1>() = -centroid * scale;
    return T;
  }
};

}  // namespace

bool projectionJacobian(const Pose& pose, const CameraIntrinsics& k, const Vec3& world_point,
                        Vec2* out_pixel, Eigen::Matrix<double, 2, 6>* out_J_pose,
                        Eigen::Matrix<double, 2, 3>* out_J_point) {
  const Vec3 p_cam = pose * world_point;
  if (p_cam.z() <= 1e-6) return false;

  const double inv_z = 1.0 / p_cam.z();
  const double inv_z2 = inv_z * inv_z;
  if (out_pixel) {
    *out_pixel = Vec2(k.fx * p_cam.x() * inv_z + k.cx, k.fy * p_cam.y() * inv_z + k.cy);
  }

  // d(pixel) / d(p_cam) for the ideal pinhole. Distortion is deliberately
  // excluded here: the pipeline undistorts observations once at ingest, so
  // every optimiser downstream sees an ideal camera.
  Eigen::Matrix<double, 2, 3> J_proj;
  J_proj << k.fx * inv_z, 0.0, -k.fx * p_cam.x() * inv_z2,
            0.0, k.fy * inv_z, -k.fy * p_cam.y() * inv_z2;

  if (out_J_pose) {
    // With the update pose <- exp(xi) * pose, the transformed point moves as
    //   p' = p + rho + omega x p = p + rho - skew(p) * omega
    // so d(p_cam)/d(rho) = I and d(p_cam)/d(omega) = -skew(p_cam).
    out_J_pose->leftCols<3>() = J_proj;
    out_J_pose->rightCols<3>() = -J_proj * skew(p_cam);
  }
  if (out_J_point) {
    // d(p_cam)/d(X_world) = R.
    *out_J_point = J_proj * pose.R;
  }
  return true;
}

bool pnpDlt(const std::vector<PointCorrespondence>& correspondences, const CameraIntrinsics& k,
            Pose* out_pose) {
  const int n = static_cast<int>(correspondences.size());
  if (n < 6) return false;

  std::vector<int> all(n);
  for (int i = 0; i < n; ++i) all[i] = i;
  const PointNormalizer norm = PointNormalizer::build(correspondences, all);

  // Two rows per correspondence, solving for the 12 entries of P row-major.
  Eigen::MatrixXd A(2 * n, 12);
  A.setZero();
  for (int i = 0; i < n; ++i) {
    const Vec3 X = norm.apply(correspondences[i].world);
    const Vec3 ray = k.unproject(correspondences[i].pixel);
    const double u = ray.x() / ray.z();
    const double v = ray.y() / ray.z();
    Vec4 Xh;
    Xh << X, 1.0;

    A.block<1, 4>(2 * i, 0) = Xh.transpose();
    A.block<1, 4>(2 * i, 8) = -u * Xh.transpose();
    A.block<1, 4>(2 * i + 1, 4) = Xh.transpose();
    A.block<1, 4>(2 * i + 1, 8) = -v * Xh.transpose();
  }

  Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
  if (svd.matrixV().cols() < 12) return false;
  const Eigen::VectorXd p = svd.matrixV().col(11);

  Mat4 P = Mat4::Identity();
  P.row(0) = p.segment<4>(0).transpose();
  P.row(1) = p.segment<4>(4).transpose();
  P.row(2) = p.segment<4>(8).transpose();
  P.row(3) << 0, 0, 0, 1;

  // Undo the point conditioning: P_world = P_normalized * T.
  const Mat34 Pw = (P * norm.matrix()).topRows<3>();

  Mat3 R_raw = Pw.leftCols<3>();
  Vec3 t_raw = Pw.rightCols<1>();

  // P is only defined up to scale, and R_raw = s * R, so the singular values of
  // R_raw are all equal to |s|. Their mean is the most stable estimate of it.
  Eigen::JacobiSVD<Mat3> rsvd(R_raw);
  const double s = rsvd.singularValues().mean();
  if (s < 1e-12) return false;
  R_raw /= s;
  t_raw /= s;

  // Fix the overall sign so that the majority of points land in front of the
  // camera: P and -P describe the same projection but opposite cheirality.
  const Mat3 R = orthonormalize(R_raw);
  int in_front = 0;
  for (const PointCorrespondence& c : correspondences) {
    if ((R * c.world + t_raw).z() > 0.0) ++in_front;
  }
  if (in_front * 2 < n) {
    *out_pose = Pose(orthonormalize(-R_raw), -t_raw);
  } else {
    *out_pose = Pose(R, t_raw);
  }
  return out_pose->R.allFinite() && out_pose->t.allFinite();
}

bool refinePoseGaussNewton(const std::vector<PointCorrespondence>& correspondences,
                           const CameraIntrinsics& k, Pose* pose, int max_iterations,
                           double huber_delta_px, double* out_rmse_px) {
  if (correspondences.size() < 3 || pose == nullptr) return false;

  double previous_cost = std::numeric_limits<double>::max();
  for (int iter = 0; iter < max_iterations; ++iter) {
    Eigen::Matrix<double, 6, 6> H = Eigen::Matrix<double, 6, 6>::Zero();
    Eigen::Matrix<double, 6, 1> g = Eigen::Matrix<double, 6, 1>::Zero();
    double cost = 0.0;
    int used = 0;

    for (const PointCorrespondence& c : correspondences) {
      Vec2 projected;
      Eigen::Matrix<double, 2, 6> J;
      if (!projectionJacobian(*pose, k, c.world, &projected, &J, nullptr)) continue;
      const Vec2 r = projected - c.pixel;
      const double e = r.norm();

      // Huber via iteratively reweighted least squares: scaling the residual
      // and Jacobian by sqrt(w) reproduces the robust normal equations.
      double w = 1.0;
      if (e > huber_delta_px) {
        w = huber_delta_px / e;
        cost += huber_delta_px * (2.0 * e - huber_delta_px);
      } else {
        cost += e * e;
      }
      const double sw = std::sqrt(w);
      const Eigen::Matrix<double, 2, 6> Jw = sw * J;
      const Vec2 rw = sw * r;
      H.noalias() += Jw.transpose() * Jw;
      g.noalias() += Jw.transpose() * rw;
      ++used;
    }
    if (used < 3) return false;

    // A small Tikhonov term keeps the step finite when the points are nearly
    // coplanar with the camera centre.
    H.diagonal().array() += 1e-9;
    const Eigen::Matrix<double, 6, 1> delta = H.ldlt().solve(-g);
    if (!delta.allFinite()) return false;
    pose->applyLeftIncrement(delta);

    if (std::abs(previous_cost - cost) < 1e-12 * (1.0 + cost) || delta.norm() < 1e-12) {
      previous_cost = cost;
      break;
    }
    previous_cost = cost;
  }

  if (out_rmse_px) {
    double sum_sq = 0.0;
    int count = 0;
    for (const PointCorrespondence& c : correspondences) {
      Vec2 projected;
      if (!projectionJacobian(*pose, k, c.world, &projected, nullptr, nullptr)) continue;
      sum_sq += (projected - c.pixel).squaredNorm();
      ++count;
    }
    *out_rmse_px = count ? std::sqrt(sum_sq / count) : 0.0;
  }
  return true;
}

PnpResult pnpRansac(const std::vector<PointCorrespondence>& correspondences,
                    const CameraIntrinsics& k, const RansacOptions& options) {
  PnpResult result;
  const int n = static_cast<int>(correspondences.size());
  if (n < 6) return result;

  const auto fit = [&correspondences, &k](const std::vector<int>& sample,
                                          std::vector<Pose>* out) {
    std::vector<PointCorrespondence> subset;
    subset.reserve(sample.size());
    for (int i : sample) subset.push_back(correspondences[i]);
    Pose pose;
    if (!pnpDlt(subset, k, &pose)) return false;
    out->push_back(pose);
    return true;
  };
  const auto residual = [&correspondences, &k](const Pose& pose, int i) {
    Vec2 projected;
    if (!k.project(pose * correspondences[i].world, &projected)) {
      return std::numeric_limits<double>::infinity();
    }
    return (projected - correspondences[i].pixel).norm();
  };
  const auto refit = [&correspondences, &k](const std::vector<uint8_t>& mask, Pose* pose) {
    std::vector<PointCorrespondence> subset;
    for (size_t i = 0; i < mask.size(); ++i) {
      if (mask[i]) subset.push_back(correspondences[i]);
    }
    if (subset.size() < 6) return false;
    Pose seeded = *pose;
    if (!pnpDlt(subset, k, &seeded)) return false;
    *pose = seeded;
    return true;
  };

  const RansacResult<Pose> fit_result = runRansac<Pose>(n, 6, options, fit, residual, refit);
  if (!fit_result.success) return result;

  result.pose = fit_result.model;
  result.inliers = fit_result.inliers;
  result.inlier_count = fit_result.inlier_count;

  // Final non-linear polish on the consensus set.
  std::vector<PointCorrespondence> inliers;
  for (int i = 0; i < n; ++i) {
    if (result.inliers[i]) inliers.push_back(correspondences[i]);
  }
  if (inliers.size() >= 4) {
    refinePoseGaussNewton(inliers, k, &result.pose, 20, options.inlier_threshold,
                          &result.rmse_px);
    // Recompute consensus after refinement.
    result.inlier_count = 0;
    for (int i = 0; i < n; ++i) {
      result.inliers[i] = residual(result.pose, i) <= options.inlier_threshold ? 1 : 0;
      result.inlier_count += result.inliers[i];
    }
  }
  result.success = result.inlier_count >= 6;
  return result;
}

}  // namespace parallax
