#include "parallax/sfm/TwoView.h"

#include "parallax/sfm/Triangulation.h"

#include <Eigen/SVD>
#include <algorithm>
#include <cmath>

namespace parallax {
namespace {

Vec3 homogeneous(const Vec2& p) { return Vec3(p.x(), p.y(), 1.0); }

/// Solve for the 9-vector spanning the null space of the constraint matrix.
///
/// ComputeFullV, not ComputeThinV: the thin decomposition returns only
/// min(rows, cols) columns of V, so for a minimal sample -- an 8x9 system for
/// the 8-point algorithm, or 8x9 for a 4-point homography -- the last column,
/// which *is* the solution, would simply not exist. Full V is always cols x
/// cols, and for the 9-column systems here it costs nothing measurable.
bool smallestSingularVector(const Eigen::MatrixXd& A, Eigen::VectorXd* out) {
  if (A.rows() < A.cols() - 1) return false;
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
  if (svd.matrixV().cols() < A.cols()) return false;
  *out = svd.matrixV().col(A.cols() - 1);
  return true;
}

}  // namespace

Mat3 normalizingTransform(const std::vector<Vec2>& points) {
  Mat3 T = Mat3::Identity();
  if (points.empty()) return T;

  Vec2 centroid = Vec2::Zero();
  for (const Vec2& p : points) centroid += p;
  centroid /= static_cast<double>(points.size());

  double mean_distance = 0.0;
  for (const Vec2& p : points) mean_distance += (p - centroid).norm();
  mean_distance /= static_cast<double>(points.size());

  // Target mean distance of sqrt(2), i.e. the "average" point sits at (1,1).
  const double scale = mean_distance > 1e-12 ? std::sqrt(2.0) / mean_distance : 1.0;
  T(0, 0) = scale;
  T(1, 1) = scale;
  T(0, 2) = -scale * centroid.x();
  T(1, 2) = -scale * centroid.y();
  return T;
}

bool fundamentalFromPairs(const std::vector<PointPair>& pairs, Mat3* out_F) {
  if (pairs.size() < 8) return false;

  std::vector<Vec2> pa, pb;
  pa.reserve(pairs.size());
  pb.reserve(pairs.size());
  for (const PointPair& p : pairs) {
    pa.push_back(p.a);
    pb.push_back(p.b);
  }
  const Mat3 Ta = normalizingTransform(pa);
  const Mat3 Tb = normalizingTransform(pb);

  // Each correspondence contributes one row of the linear system x_b^T F x_a = 0.
  Eigen::MatrixXd A(static_cast<int>(pairs.size()), 9);
  for (size_t i = 0; i < pairs.size(); ++i) {
    const Vec3 a = Ta * homogeneous(pairs[i].a);
    const Vec3 b = Tb * homogeneous(pairs[i].b);
    A.row(static_cast<int>(i)) << b.x() * a.x(), b.x() * a.y(), b.x(), b.y() * a.x(),
        b.y() * a.y(), b.y(), a.x(), a.y(), 1.0;
  }

  Eigen::VectorXd f;
  if (!smallestSingularVector(A, &f)) return false;

  Mat3 F;
  F << f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8);

  // Enforce rank 2: a valid fundamental matrix is singular, and the linear
  // solution generally is not. Zeroing the smallest singular value is the
  // closest rank-2 matrix in Frobenius norm.
  Eigen::JacobiSVD<Mat3> svd(F, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Vec3 s = svd.singularValues();
  s(2) = 0.0;
  F = svd.matrixU() * s.asDiagonal() * svd.matrixV().transpose();

  // Undo the normalisation.
  F = Tb.transpose() * F * Ta;
  const double n = F.norm();
  if (n < 1e-14) return false;
  *out_F = F / n;
  return true;
}

Mat3 essentialFromFundamental(const Mat3& F, const CameraIntrinsics& ka,
                              const CameraIntrinsics& kb) {
  return kb.matrix().transpose() * F * ka.matrix();
}

Mat3 projectToEssential(const Mat3& E) {
  // A valid essential matrix has two equal non-zero singular values and one
  // zero; averaging the first two is the nearest such matrix.
  Eigen::JacobiSVD<Mat3> svd(E, Eigen::ComputeFullU | Eigen::ComputeFullV);
  const Vec3 s = svd.singularValues();
  const double mean = 0.5 * (s(0) + s(1));
  const Vec3 fixed(mean, mean, 0.0);
  return svd.matrixU() * fixed.asDiagonal() * svd.matrixV().transpose();
}

bool essentialFromNormalizedPairs(const std::vector<PointPair>& normalized_pairs, Mat3* out_E) {
  Mat3 E;
  if (!fundamentalFromPairs(normalized_pairs, &E)) return false;
  E = projectToEssential(E);
  const double n = E.norm();
  if (n < 1e-14) return false;
  *out_E = E / n;
  return true;
}

void decomposeEssential(const Mat3& E, std::array<Pose, 4>* out_candidates) {
  Eigen::JacobiSVD<Mat3> svd(E, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Mat3 U = svd.matrixU();
  Mat3 V = svd.matrixV();
  // Both U and V must be rotations for the decomposition to yield valid poses.
  if (U.determinant() < 0.0) U.col(2) *= -1.0;
  if (V.determinant() < 0.0) V.col(2) *= -1.0;

  Mat3 W;
  W << 0, -1, 0,
       1,  0, 0,
       0,  0, 1;

  const Mat3 R1 = U * W * V.transpose();
  const Mat3 R2 = U * W.transpose() * V.transpose();
  // The translation direction is the third column of U, up to sign.
  const Vec3 t = U.col(2).normalized();

  (*out_candidates)[0] = Pose(R1, t);
  (*out_candidates)[1] = Pose(R1, -t);
  (*out_candidates)[2] = Pose(R2, t);
  (*out_candidates)[3] = Pose(R2, -t);
}

double sampsonDistance(const Mat3& F, const PointPair& pair) {
  const Vec3 a = homogeneous(pair.a);
  const Vec3 b = homogeneous(pair.b);
  const double num = b.dot(F * a);
  const Vec3 Fa = F * a;
  const Vec3 Ftb = F.transpose() * b;
  // The denominator is the norm of the gradient of the algebraic residual, which
  // converts it to a first-order geometric (pixel) distance.
  const double denom = Fa.x() * Fa.x() + Fa.y() * Fa.y() + Ftb.x() * Ftb.x() + Ftb.y() * Ftb.y();
  if (denom < 1e-18) return std::numeric_limits<double>::infinity();
  return std::abs(num) / std::sqrt(denom);
}

double symmetricEpipolarDistance(const Mat3& F, const PointPair& pair) {
  const Vec3 a = homogeneous(pair.a);
  const Vec3 b = homogeneous(pair.b);
  const double num = b.dot(F * a);
  const Vec3 Fa = F * a;
  const Vec3 Ftb = F.transpose() * b;
  const double da2 = Fa.x() * Fa.x() + Fa.y() * Fa.y();
  const double db2 = Ftb.x() * Ftb.x() + Ftb.y() * Ftb.y();
  if (da2 < 1e-18 || db2 < 1e-18) return std::numeric_limits<double>::infinity();
  return 0.5 * std::abs(num) * (1.0 / std::sqrt(da2) + 1.0 / std::sqrt(db2));
}

bool recoverPoseFromEssential(const Mat3& E, const std::vector<PointPair>& pairs,
                              const CameraIntrinsics& ka, const CameraIntrinsics& kb,
                              Pose* out_pose, std::vector<uint8_t>* out_inliers) {
  if (pairs.empty()) return false;

  std::array<Pose, 4> candidates;
  decomposeEssential(E, &candidates);

  // The first camera defines the world frame.
  const Pose pose_a = Pose::Identity();
  int best_count = -1;
  size_t best_index = 0;
  std::vector<uint8_t> best_mask;

  for (size_t c = 0; c < candidates.size(); ++c) {
    std::vector<uint8_t> mask(pairs.size(), 0);
    int count = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
      // Relax the thresholds here: this is a discrete 4-way vote on which
      // candidate has points in front of both cameras, not a quality gate.
      const TriangulationResult tri = triangulateDlt(pose_a, ka, pairs[i].a, candidates[c], kb,
                                                     pairs[i].b, 0.0, 1e9);
      if (tri.valid) {
        mask[i] = 1;
        ++count;
      }
    }
    if (count > best_count) {
      best_count = count;
      best_index = c;
      best_mask = std::move(mask);
    }
  }
  if (best_count <= 0) return false;

  *out_pose = candidates[best_index];
  if (out_inliers) *out_inliers = best_mask;
  return true;
}

bool homographyFromPairs(const std::vector<PointPair>& pairs, Mat3* out_H) {
  if (pairs.size() < 4) return false;

  std::vector<Vec2> pa, pb;
  pa.reserve(pairs.size());
  pb.reserve(pairs.size());
  for (const PointPair& p : pairs) {
    pa.push_back(p.a);
    pb.push_back(p.b);
  }
  const Mat3 Ta = normalizingTransform(pa);
  const Mat3 Tb = normalizingTransform(pb);

  // Two rows per correspondence from the cross product x_b x (H x_a) = 0.
  Eigen::MatrixXd A(static_cast<int>(pairs.size()) * 2, 9);
  for (size_t i = 0; i < pairs.size(); ++i) {
    const Vec3 a = Ta * homogeneous(pairs[i].a);
    const Vec3 b = Tb * homogeneous(pairs[i].b);
    const int r = static_cast<int>(i) * 2;
    A.row(r) << -a.x(), -a.y(), -1.0, 0, 0, 0, b.x() * a.x(), b.x() * a.y(), b.x();
    A.row(r + 1) << 0, 0, 0, -a.x(), -a.y(), -1.0, b.y() * a.x(), b.y() * a.y(), b.y();
  }

  Eigen::VectorXd h;
  if (!smallestSingularVector(A, &h)) return false;

  Mat3 H;
  H << h(0), h(1), h(2), h(3), h(4), h(5), h(6), h(7), h(8);
  H = Tb.inverse() * H * Ta;
  if (std::abs(H(2, 2)) > 1e-12) H /= H(2, 2);
  *out_H = H;
  return true;
}

double homographyTransferError(const Mat3& H, const PointPair& pair) {
  const Vec3 forward = H * homogeneous(pair.a);
  if (std::abs(forward.z()) < 1e-12) return std::numeric_limits<double>::infinity();
  const double e1 = (forward.head<2>() / forward.z() - pair.b).norm();

  // Symmetric: also measure the backward transfer, so a homography that happens
  // to collapse one direction cannot score well.
  const Mat3 Hinv = H.inverse();
  if (!Hinv.allFinite()) return std::numeric_limits<double>::infinity();
  const Vec3 backward = Hinv * homogeneous(pair.b);
  if (std::abs(backward.z()) < 1e-12) return std::numeric_limits<double>::infinity();
  const double e2 = (backward.head<2>() / backward.z() - pair.a).norm();
  return 0.5 * (e1 + e2);
}

TwoViewGeometry estimateTwoView(const std::vector<PointPair>& pairs, const CameraIntrinsics& ka,
                                const CameraIntrinsics& kb, const RansacOptions& options) {
  TwoViewGeometry result;
  const int n = static_cast<int>(pairs.size());
  if (n < 8) return result;

  // Normalise to calibrated rays once: estimating E directly keeps the
  // threshold interpretable after we scale it back to pixels below.
  std::vector<PointPair> normalized(pairs.size());
  for (size_t i = 0; i < pairs.size(); ++i) {
    const Vec3 ra = ka.unproject(pairs[i].a);
    const Vec3 rb = kb.unproject(pairs[i].b);
    normalized[i].a = ra.head<2>() / ra.z();
    normalized[i].b = rb.head<2>() / rb.z();
  }

  // Convert the pixel threshold into normalised units using the mean focal
  // length, so callers can keep thinking in pixels.
  const double focal = 0.25 * (ka.fx + ka.fy + kb.fx + kb.fy);
  RansacOptions ransac = options;
  ransac.inlier_threshold = options.inlier_threshold / std::max(1.0, focal);

  const auto fit = [&normalized](const std::vector<int>& sample, std::vector<Mat3>* out) {
    std::vector<PointPair> subset;
    subset.reserve(sample.size());
    for (int i : sample) subset.push_back(normalized[i]);
    Mat3 E;
    if (!essentialFromNormalizedPairs(subset, &E)) return false;
    out->push_back(E);
    return true;
  };
  const auto residual = [&normalized](const Mat3& E, int i) {
    return sampsonDistance(E, normalized[i]);
  };
  const auto refit = [&normalized](const std::vector<uint8_t>& mask, Mat3* model) {
    std::vector<PointPair> subset;
    for (size_t i = 0; i < mask.size(); ++i) {
      if (mask[i]) subset.push_back(normalized[i]);
    }
    return essentialFromNormalizedPairs(subset, model);
  };

  RansacOptions essential_options = ransac;
  const RansacResult<Mat3> fit_result =
      runRansac<Mat3>(n, 8, essential_options, fit, residual, refit);
  if (!fit_result.success || fit_result.inlier_count < 8) return result;

  result.essential = fit_result.model;
  result.inliers = fit_result.inliers;
  result.inlier_count = fit_result.inlier_count;
  // F = K_b^-T E K_a^-1
  result.fundamental = kb.matrix().transpose().inverse() * result.essential * ka.matrix().inverse();

  std::vector<double> errors;
  errors.reserve(result.inlier_count);
  std::vector<PointPair> inlier_pairs, inlier_normalized;
  for (int i = 0; i < n; ++i) {
    if (!result.inliers[i]) continue;
    inlier_pairs.push_back(pairs[i]);
    inlier_normalized.push_back(normalized[i]);
    errors.push_back(sampsonDistance(result.essential, normalized[i]) * focal);
  }
  if (!errors.empty()) {
    std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
    result.median_sampson = errors[errors.size() / 2];
  }

  // Degeneracy check. If a homography explains nearly all the epipolar inliers,
  // the scene is planar or the motion is a pure rotation; in both cases the
  // essential matrix is not uniquely determined and the recovered baseline is
  // meaningless. The caller must pick a different pair to initialise from.
  if (inlier_normalized.size() >= 4) {
    const auto hfit = [&inlier_normalized](const std::vector<int>& sample,
                                           std::vector<Mat3>* out) {
      std::vector<PointPair> subset;
      for (int i : sample) subset.push_back(inlier_normalized[i]);
      Mat3 H;
      if (!homographyFromPairs(subset, &H)) return false;
      out->push_back(H);
      return true;
    };
    const auto hres = [&inlier_normalized](const Mat3& H, int i) {
      return homographyTransferError(H, inlier_normalized[i]);
    };
    RansacOptions hopts = ransac;
    hopts.max_iterations = std::min(600, ransac.max_iterations);
    const RansacResult<Mat3> h =
        runRansac<Mat3>(static_cast<int>(inlier_normalized.size()), 4, hopts, hfit, hres);
    result.planarity = h.inlierRatio(inlier_normalized.size());
  }

  std::vector<uint8_t> cheirality;
  if (!recoverPoseFromEssential(result.essential, inlier_pairs, ka, kb, &result.relative_pose,
                                &cheirality)) {
    return result;
  }
  result.success = true;
  return result;
}

}  // namespace parallax
