#include "parallax/geometry/SE3.h"

#include <Eigen/SVD>
#include <cmath>

namespace parallax {

Mat3 so3Exp(const Vec3& omega) {
  const double theta2 = omega.squaredNorm();
  const Mat3 W = skew(omega);
  // Near zero the trigonometric coefficients are evaluated by Taylor series to
  // avoid catastrophic cancellation in sin(t)/t and (1-cos t)/t^2.
  if (theta2 < 1e-12) {
    return Mat3::Identity() + W + 0.5 * W * W;
  }
  const double theta = std::sqrt(theta2);
  const double a = std::sin(theta) / theta;
  const double b = (1.0 - std::cos(theta)) / theta2;
  return Mat3::Identity() + a * W + b * W * W;
}

Vec3 so3Log(const Mat3& R) {
  // The antisymmetric part of R equals 2 * sin(theta) * axis.
  const Vec3 anti(R(2, 1) - R(1, 2), R(0, 2) - R(2, 0), R(1, 0) - R(0, 1));
  const double s = 0.5 * anti.norm();                              // |sin(theta)|
  const double c = std::max(-1.0, std::min(1.0, 0.5 * (R.trace() - 1.0)));  // cos(theta)
  // atan2 stays well conditioned across the whole range, unlike acos(c) which
  // loses precision as c approaches +/-1.
  const double theta = std::atan2(s, c);

  if (theta < 1e-8) {
    // First order: R ~ I + skew(w), so the antisymmetric part is simply 2w.
    return anti * 0.5;
  }
  if (theta < kPi - 1e-4) {
    // sin(theta) is comfortably away from zero, so the axis is well determined
    // by the antisymmetric part alone.
    return anti * (theta / (2.0 * s));
  }
  // Near theta = pi the antisymmetric part vanishes and carries no usable axis
  // information. Recover the axis from the symmetric part instead:
  //     (R + R^T) / 2 = cos(theta) I + (1 - cos(theta)) a a^T
  // so S below tends to a a^T as theta -> pi. Symmetrising first is what makes
  // this accurate: using (R + I) / 2 directly leaves an O(sin theta) skew term
  // that contaminates the axis to first order in (pi - theta).
  const Mat3 S = 0.5 * (0.5 * (R + R.transpose()) + Mat3::Identity());
  int best = 0;
  for (int i = 1; i < 3; ++i) {
    if (S(i, i) > S(best, best)) best = i;
  }
  Vec3 axis = S.col(best);  // == a * a[best], best conditioned column
  const double n = axis.norm();
  if (n < 1e-12) return Vec3::Zero();
  axis /= n;
  // Resolve the sign from the residual antisymmetric part. Exactly at pi both
  // signs describe the same rotation, so either answer is correct.
  if (axis.dot(anti) < 0.0) axis = -axis;
  return axis * theta;
}

Mat3 orthonormalize(const Mat3& m) {
  Eigen::JacobiSVD<Mat3> svd(m, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Mat3 R = svd.matrixU() * svd.matrixV().transpose();
  if (R.determinant() < 0.0) {
    Mat3 U = svd.matrixU();
    U.col(2) *= -1.0;
    R = U * svd.matrixV().transpose();
  }
  return R;
}

Pose Pose::fromWorldFromCamera(const Mat4& world_from_camera) {
  const Mat3 Rwc = world_from_camera.topLeftCorner<3, 3>();
  const Vec3 twc = world_from_camera.topRightCorner<3, 1>();
  const Mat3 R = orthonormalize(Rwc).transpose();
  return Pose(R, -R * twc);
}

Pose Pose::exp(const Eigen::Matrix<double, 6, 1>& xi) {
  const Vec3 rho = xi.head<3>();
  const Vec3 omega = xi.tail<3>();
  const double theta2 = omega.squaredNorm();
  const Mat3 W = skew(omega);
  const Mat3 R = so3Exp(omega);

  // Left Jacobian V of SE(3): t = V * rho.
  Mat3 V;
  if (theta2 < 1e-12) {
    V = Mat3::Identity() + 0.5 * W + (1.0 / 6.0) * W * W;
  } else {
    const double theta = std::sqrt(theta2);
    const double a = (1.0 - std::cos(theta)) / theta2;
    const double b = (theta - std::sin(theta)) / (theta2 * theta);
    V = Mat3::Identity() + a * W + b * W * W;
  }
  return Pose(R, V * rho);
}

Eigen::Matrix<double, 6, 1> Pose::log() const {
  const Vec3 omega = so3Log(R);
  const double theta2 = omega.squaredNorm();
  const Mat3 W = skew(omega);

  Mat3 Vinv;
  if (theta2 < 1e-12) {
    Vinv = Mat3::Identity() - 0.5 * W + (1.0 / 12.0) * W * W;
  } else {
    const double theta = std::sqrt(theta2);
    const double half = 0.5 * theta;
    // c = (1 - (theta/2) cot(theta/2)) / theta^2
    const double c = (1.0 - half * std::cos(half) / std::sin(half)) / theta2;
    Vinv = Mat3::Identity() - 0.5 * W + c * W * W;
  }
  Eigen::Matrix<double, 6, 1> xi;
  xi.head<3>() = Vinv * t;
  xi.tail<3>() = omega;
  return xi;
}

void Pose::applyLeftIncrement(const Eigen::Matrix<double, 6, 1>& delta) {
  const Pose inc = Pose::exp(delta);
  R = inc.R * R;
  t = inc.R * t + inc.t;
  normalize();
}

Pose Pose::interpolate(const Pose& a, const Pose& b, double alpha) {
  // Interpolate in the body frame of `a`: a * exp(alpha * log(a^-1 b)).
  const Pose rel = a.inverse() * b;
  return a * Pose::exp(rel.log() * alpha);
}

double Pose::rotationAngle(const Pose& a, const Pose& b) {
  return so3Log(a.R * b.R.transpose()).norm();
}

Mat3 averageRotations(const std::vector<Mat3>& rotations) {
  if (rotations.empty()) return Mat3::Identity();
  Mat3 acc = Mat3::Zero();
  for (const Mat3& r : rotations) acc += r;
  return orthonormalize(acc / static_cast<double>(rotations.size()));
}

}  // namespace parallax
