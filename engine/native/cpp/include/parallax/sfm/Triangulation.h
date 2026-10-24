// Point triangulation from two or more views.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"

namespace parallax {

/// One observation of a 3D point: which camera saw it, and where.
struct Observation {
  int camera = -1;
  int point = -1;
  Vec2 pixel = Vec2::Zero();
  float weight = 1.0f;
};

struct TriangulationResult {
  Vec3 point = Vec3::Zero();
  double parallax_deg = 0.0;        ///< angle between the two extreme rays
  double max_reprojection_px = 0.0;
  bool valid = false;
};

/**
 * Two-view triangulation by the linear DLT (Hartley & Zisserman 12.2), followed
 * by validation: the point must lie in front of both cameras, subtend enough
 * parallax to be well conditioned, and reproject within tolerance.
 *
 * Low parallax is the dominant failure mode in handheld capture -- a point seen
 * from two nearly identical viewpoints has a depth uncertainty that grows as
 * 1/sin(parallax), so accepting it would poison bundle adjustment.
 */
TriangulationResult triangulateDlt(const Pose& pose_a, const CameraIntrinsics& ka,
                                   const Vec2& px_a, const Pose& pose_b,
                                   const CameraIntrinsics& kb, const Vec2& px_b,
                                   double min_parallax_deg = 1.0,
                                   double max_reprojection_px = 4.0);

/// Multi-view triangulation: the same DLT stacked over every observation, which
/// is the least-squares optimum in the algebraic sense and a good initialiser
/// for bundle adjustment.
TriangulationResult triangulateMultiView(const std::vector<Pose>& poses,
                                         const std::vector<CameraIntrinsics>& intrinsics,
                                         const std::vector<Observation>& observations,
                                         double min_parallax_deg = 1.0,
                                         double max_reprojection_px = 4.0);

/// Angle in degrees between the rays from two camera centres to a world point.
double parallaxDegrees(const Vec3& center_a, const Vec3& center_b, const Vec3& point);

/// Reprojection error in pixels; returns infinity if the point is behind the camera.
double reprojectionError(const Pose& pose, const CameraIntrinsics& k, const Vec3& point,
                         const Vec2& observed);

}  // namespace parallax
