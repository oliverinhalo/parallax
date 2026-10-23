// Rigid body transforms on SO(3)/SE(3) with exponential/logarithm maps.
//
// Conventions used throughout the engine:
//   * A Pose holds the camera-from-world transform (R, t) so that
//       p_cam = R * p_world + t
//     This is the convention of multi-view geometry textbooks and keeps
//     projection cheap. ARCore hands us world-from-camera, so the ingest
//     stage inverts once at the boundary (see Pose::inverse).
//   * Tangent vectors are ordered [translation(3), rotation(3)], matching the
//     layout of the bundle adjustment Jacobians.
#pragma once

#include "parallax/core/Types.h"

namespace parallax {

/// Right Jacobian-free SO(3) exponential map (Rodrigues' formula).
Mat3 so3Exp(const Vec3& omega);

/// SO(3) logarithm; returns the rotation vector whose norm is the angle.
Vec3 so3Log(const Mat3& R);

/// Skew-symmetric matrix such that skew(a) * b == a.cross(b).
inline Mat3 skew(const Vec3& a) {
  Mat3 m;
  m <<     0.0, -a.z(),  a.y(),
        a.z(),     0.0, -a.x(),
       -a.y(),  a.x(),     0.0;
  return m;
}

/// Project an arbitrary 3x3 matrix onto the closest rotation (SVD, det=+1).
Mat3 orthonormalize(const Mat3& m);

/// Camera-from-world rigid transform.
struct Pose {
  Mat3 R = Mat3::Identity();
  Vec3 t = Vec3::Zero();

  Pose() = default;
  Pose(const Mat3& rotation, const Vec3& translation) : R(rotation), t(translation) {}

  static Pose Identity() { return Pose(); }

  /// Build from a world-from-camera 4x4 (the layout ARCore and glTF use).
  static Pose fromWorldFromCamera(const Mat4& world_from_camera);

  /// Camera centre in world coordinates: C = -R^T t.
  Vec3 center() const { return -R.transpose() * t; }

  /// Viewing direction (camera +Z axis) expressed in world coordinates.
  Vec3 forward() const { return R.transpose() * Vec3::UnitZ(); }

  Vec3 operator*(const Vec3& p_world) const { return R * p_world + t; }

  /// Composition: (a * b) transforms by b first, then a.
  Pose operator*(const Pose& other) const {
    return Pose(R * other.R, R * other.t + t);
  }

  Pose inverse() const {
    const Mat3 Rt = R.transpose();
    return Pose(Rt, -Rt * t);
  }

  Mat34 matrix34() const {
    Mat34 m;
    m.leftCols<3>() = R;
    m.rightCols<1>() = t;
    return m;
  }

  Mat4 matrix() const {
    Mat4 m = Mat4::Identity();
    m.topLeftCorner<3, 4>() = matrix34();
    return m;
  }

  /// World-from-camera 4x4, the form renderers and glTF want.
  Mat4 worldFromCamera() const { return inverse().matrix(); }

  /// Exponential map from a 6-vector [translation, rotation].
  static Pose exp(const Eigen::Matrix<double, 6, 1>& xi);

  /// Logarithm into a 6-vector [translation, rotation].
  Eigen::Matrix<double, 6, 1> log() const;

  /// Apply a left-multiplied increment: this <- exp(delta) * this.
  void applyLeftIncrement(const Eigen::Matrix<double, 6, 1>& delta);

  /// Re-project R onto SO(3), undoing drift accumulated by repeated updates.
  void normalize() { R = orthonormalize(R); }

  /// Geodesic interpolation between two poses, alpha in [0,1].
  static Pose interpolate(const Pose& a, const Pose& b, double alpha);

  /// Angle in radians between the rotations of two poses.
  static double rotationAngle(const Pose& a, const Pose& b);

  /// Distance between camera centres.
  static double baseline(const Pose& a, const Pose& b) {
    return (a.center() - b.center()).norm();
  }
};

/// Average a set of rotations (chordal L2 mean via SVD projection).
Mat3 averageRotations(const std::vector<Mat3>& rotations);

}  // namespace parallax
