// Plane extraction from an oriented point cloud.
//
// Used in two places: room mode fits the floor, ceiling and walls in order to
// build a floor plan, and object mode finds the supporting surface so it can be
// cut away. ARCore also reports planes directly during capture; those are fed in
// as priors rather than replacing this, because ARCore's planes are coarse and
// stop at the edge of what it tracked.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/mesh/Mesh.h"

namespace parallax {

/// A plane in Hesse normal form: points p satisfy normal . p + offset == 0.
struct Plane {
  Vec3 normal = Vec3::UnitY();
  double offset = 0.0;

  double signedDistance(const Vec3& p) const { return normal.dot(p) + offset; }
  /// Closest point on the plane to p.
  Vec3 project(const Vec3& p) const { return p - normal * signedDistance(p); }

  static Plane fromPointAndNormal(const Vec3& point, const Vec3& normal) {
    Plane plane;
    plane.normal = normal.normalized();
    plane.offset = -plane.normal.dot(point);
    return plane;
  }
  /// Flip so the normal points towards `viewpoint`.
  void orientTowards(const Vec3& viewpoint) {
    if (signedDistance(viewpoint) < 0.0) {
      normal = -normal;
      offset = -offset;
    }
  }
};

/// Least-squares plane through a point set (the smallest-eigenvalue direction
/// of the scatter matrix). Returns false for fewer than three points or a
/// degenerate (collinear) set.
bool fitPlane(const std::vector<Vec3>& points, Plane* out_plane,
              double* out_rms = nullptr);

enum class PlaneRole {
  Unknown,
  Floor,
  Ceiling,
  Wall,
};

struct DetectedPlane {
  Plane plane;
  PlaneRole role = PlaneRole::Unknown;
  std::vector<uint32_t> inliers;   ///< indices into the input point array
  double area_estimate = 0.0;      ///< m^2, from the inlier extent
  double rms_error = 0.0;
  /// Extent of the inliers within the plane, along two in-plane axes.
  Vec3 centroid = Vec3::Zero();
  Vec3 axis_u = Vec3::UnitX();
  Vec3 axis_v = Vec3::UnitZ();
  double extent_u = 0.0;
  double extent_v = 0.0;
};

struct PlaneDetectionOptions {
  /// A point is an inlier if it lies within this distance of the plane.
  double distance_threshold = 0.035;
  /// And if its normal agrees with the plane's to within this angle.
  double normal_threshold_deg = 22.0;
  /// Minimum inlier count for a plane to be reported at all.
  int min_inliers = 150;
  /// Minimum fraction of the remaining points; stops the search producing a
  /// long tail of tiny slivers.
  double min_inlier_fraction = 0.012;
  int max_planes = 16;
  int ransac_iterations = 900;
  /// Gravity direction, which ARCore supplies. Used to classify roles.
  Vec3 up = Vec3::UnitY();
  /// A plane is horizontal if its normal is within this angle of gravity.
  double horizontal_tolerance_deg = 15.0;
  /// And vertical if its normal is within this angle of the horizontal plane.
  double vertical_tolerance_deg = 15.0;
  uint64_t seed = 0xC0FFEEULL;
};

/**
 * Sequential RANSAC plane extraction: find the largest plane, remove its
 * inliers, repeat. Normals are required -- without them a RANSAC plane happily
 * fits points from two different surfaces that happen to be coplanar in
 * projection, which in a room means merging a wall with the floor behind it.
 */
std::vector<DetectedPlane> detectPlanes(const std::vector<Vec3f>& points,
                                        const std::vector<Vec3f>& normals,
                                        const PlaneDetectionOptions& options = {});

/// Convenience overload taking a mesh (uses vertices and vertex normals).
std::vector<DetectedPlane> detectPlanes(const Mesh& mesh,
                                        const PlaneDetectionOptions& options = {});

/// Classify a plane's role from its normal and position relative to the others.
void classifyPlaneRoles(std::vector<DetectedPlane>* planes,
                        const PlaneDetectionOptions& options);

/**
 * Rotation that takes the detected floor normal onto `options.up`, so the model
 * can be auto-levelled. Returns identity when no floor was found.
 */
Mat3 levellingRotation(const std::vector<DetectedPlane>& planes,
                       const PlaneDetectionOptions& options = {});

}  // namespace parallax
