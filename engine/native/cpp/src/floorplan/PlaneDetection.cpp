#include "parallax/floorplan/PlaneDetection.h"

#include <Eigen/Eigenvalues>
#include <algorithm>
#include <cmath>

#include "parallax/testing/SyntheticScene.h"  // Rng

namespace parallax {
namespace {

inline Vec3 toDouble(const Vec3f& v) { return Vec3(v.x(), v.y(), v.z()); }

/// Build two orthonormal in-plane axes for a given normal.
void planeAxes(const Vec3& normal, Vec3* u, Vec3* v) {
  // Pick the world axis least aligned with the normal so the cross product is
  // well conditioned.
  const Vec3 n = normal.normalized();
  Vec3 helper = Vec3::UnitX();
  if (std::abs(n.x()) > 0.9) helper = Vec3::UnitY();
  *u = n.cross(helper).normalized();
  *v = n.cross(*u).normalized();
}

}  // namespace

bool fitPlane(const std::vector<Vec3>& points, Plane* out_plane, double* out_rms) {
  if (points.size() < 3) return false;

  Vec3 centroid = Vec3::Zero();
  for (const Vec3& p : points) centroid += p;
  centroid /= static_cast<double>(points.size());

  // Scatter matrix; the eigenvector for the smallest eigenvalue is the normal.
  Mat3 scatter = Mat3::Zero();
  for (const Vec3& p : points) {
    const Vec3 d = p - centroid;
    scatter += d * d.transpose();
  }
  Eigen::SelfAdjointEigenSolver<Mat3> solver(scatter);
  if (solver.info() != Eigen::Success) return false;

  const Vec3 eigenvalues = solver.eigenvalues();
  // SelfAdjointEigenSolver returns eigenvalues in increasing order.
  const Vec3 normal = solver.eigenvectors().col(0);
  // Degenerate: the points are collinear, so two eigenvalues are ~0 and the
  // normal is not determined.
  if (eigenvalues(1) < 1e-14 || !normal.allFinite()) return false;

  *out_plane = Plane::fromPointAndNormal(centroid, normal);
  if (out_rms) {
    double sum_sq = 0.0;
    for (const Vec3& p : points) {
      const double d = out_plane->signedDistance(p);
      sum_sq += d * d;
    }
    *out_rms = std::sqrt(sum_sq / points.size());
  }
  return true;
}

std::vector<DetectedPlane> detectPlanes(const std::vector<Vec3f>& points,
                                        const std::vector<Vec3f>& normals,
                                        const PlaneDetectionOptions& options) {
  std::vector<DetectedPlane> result;
  const size_t n = points.size();
  if (n < static_cast<size_t>(std::max(3, options.min_inliers))) return result;
  const bool have_normals = normals.size() == n;

  std::vector<uint8_t> consumed(n, 0);
  size_t remaining = n;
  const double cos_normal = std::cos(deg2rad(options.normal_threshold_deg));
  testing::Rng rng(options.seed);

  for (int plane_index = 0; plane_index < options.max_planes; ++plane_index) {
    const int min_required =
        std::max<int>(options.min_inliers,
                      static_cast<int>(options.min_inlier_fraction * remaining));
    if (remaining < static_cast<size_t>(min_required)) break;

    // Index of the points still available, so sampling stays efficient as the
    // cloud is consumed.
    std::vector<uint32_t> available;
    available.reserve(remaining);
    for (size_t i = 0; i < n; ++i) {
      if (!consumed[i]) available.push_back(static_cast<uint32_t>(i));
    }
    if (available.size() < 3) break;

    Plane best_plane;
    std::vector<uint32_t> best_inliers;

    for (int iter = 0; iter < options.ransac_iterations; ++iter) {
      // Three distinct samples.
      uint32_t a = available[static_cast<size_t>(rng.uniform() * available.size()) %
                             available.size()];
      uint32_t b = available[static_cast<size_t>(rng.uniform() * available.size()) %
                             available.size()];
      uint32_t c = available[static_cast<size_t>(rng.uniform() * available.size()) %
                             available.size()];
      if (a == b || b == c || a == c) continue;

      const Vec3 pa = toDouble(points[a]), pb = toDouble(points[b]), pc = toDouble(points[c]);
      Vec3 normal = (pb - pa).cross(pc - pa);
      const double area2 = normal.norm();
      // Reject near-collinear samples: they define the plane very poorly.
      if (area2 < 1e-8) continue;
      normal /= area2;

      // Seed the orientation from the sample normals so the plane's normal is
      // consistently outward-facing.
      if (have_normals) {
        const Vec3 reference = toDouble(normals[a]);
        if (reference.dot(normal) < 0.0) normal = -normal;
      }
      const Plane candidate = Plane::fromPointAndNormal(pa, normal);

      std::vector<uint32_t> inliers;
      inliers.reserve(available.size() / 4);
      for (uint32_t i : available) {
        if (std::abs(candidate.signedDistance(toDouble(points[i]))) >
            options.distance_threshold) {
          continue;
        }
        // The normal test is what stops two coplanar-in-projection surfaces
        // being merged into one plane.
        //
        // Note the absence of std::abs: the comparison must be *signed*. Two
        // surfaces 2 cm apart facing opposite ways (a wall and the far side of
        // the same wall, a floor and the underside of a shelf) are within any
        // sensible distance threshold, and taking the absolute value would
        // accept a normal pointing 180 degrees the wrong way, merging them.
        if (have_normals && toDouble(normals[i]).dot(candidate.normal) < cos_normal) {
          continue;
        }
        inliers.push_back(i);
      }
      if (inliers.size() > best_inliers.size()) {
        best_inliers = std::move(inliers);
        best_plane = candidate;
      }
    }

    if (static_cast<int>(best_inliers.size()) < min_required) break;

    // Refit to the consensus set, then re-gather: the minimal-sample plane is
    // only ever an approximation.
    std::vector<Vec3> inlier_points;
    inlier_points.reserve(best_inliers.size());
    for (uint32_t i : best_inliers) inlier_points.push_back(toDouble(points[i]));
    Plane refined = best_plane;
    double rms = 0.0;
    if (fitPlane(inlier_points, &refined, &rms)) {
      // Keep the orientation of the RANSAC plane; fitPlane's sign is arbitrary.
      if (refined.normal.dot(best_plane.normal) < 0.0) {
        refined.normal = -refined.normal;
        refined.offset = -refined.offset;
      }
      std::vector<uint32_t> regathered;
      for (uint32_t i : available) {
        if (std::abs(refined.signedDistance(toDouble(points[i]))) >
            options.distance_threshold) {
          continue;
        }
        if (have_normals && toDouble(normals[i]).dot(refined.normal) < cos_normal) {
          continue;
        }
        regathered.push_back(i);
      }
      if (regathered.size() >= best_inliers.size()) {
        best_inliers = std::move(regathered);
        best_plane = refined;
      }
    }

    DetectedPlane detected;
    detected.plane = best_plane;
    detected.inliers = best_inliers;
    detected.rms_error = rms;

    // In-plane extent, used for the area estimate and for the floor plan.
    planeAxes(best_plane.normal, &detected.axis_u, &detected.axis_v);
    Vec3 centroid = Vec3::Zero();
    for (uint32_t i : best_inliers) centroid += toDouble(points[i]);
    centroid /= static_cast<double>(best_inliers.size());
    detected.centroid = centroid;

    double min_u = 1e300, max_u = -1e300, min_v = 1e300, max_v = -1e300;
    for (uint32_t i : best_inliers) {
      const Vec3 d = toDouble(points[i]) - centroid;
      const double u = d.dot(detected.axis_u);
      const double v = d.dot(detected.axis_v);
      min_u = std::min(min_u, u);
      max_u = std::max(max_u, u);
      min_v = std::min(min_v, v);
      max_v = std::max(max_v, v);
    }
    detected.extent_u = max_u - min_u;
    detected.extent_v = max_v - min_v;
    // A bounding-box area over-states a non-rectangular region, so scale by the
    // fraction of the box the inliers actually occupy, estimated from density.
    detected.area_estimate = detected.extent_u * detected.extent_v;

    for (uint32_t i : best_inliers) {
      if (!consumed[i]) {
        consumed[i] = 1;
        --remaining;
      }
    }
    result.push_back(std::move(detected));
  }

  classifyPlaneRoles(&result, options);
  return result;
}

std::vector<DetectedPlane> detectPlanes(const Mesh& mesh,
                                        const PlaneDetectionOptions& options) {
  return detectPlanes(mesh.vertices, mesh.normals, options);
}

void classifyPlaneRoles(std::vector<DetectedPlane>* planes,
                        const PlaneDetectionOptions& options) {
  if (planes == nullptr || planes->empty()) return;
  const Vec3 up = options.up.normalized();
  const double cos_horizontal = std::cos(deg2rad(options.horizontal_tolerance_deg));
  const double sin_vertical = std::sin(deg2rad(options.vertical_tolerance_deg));

  // First pass: horizontal or vertical.
  std::vector<int> horizontal;
  for (size_t i = 0; i < planes->size(); ++i) {
    DetectedPlane& p = (*planes)[i];
    const double alignment = p.plane.normal.dot(up);
    if (std::abs(alignment) >= cos_horizontal) {
      horizontal.push_back(static_cast<int>(i));
    } else if (std::abs(alignment) <= sin_vertical) {
      p.role = PlaneRole::Wall;
    } else {
      p.role = PlaneRole::Unknown;  // a sloped surface: a ramp, a desk, a sofa
    }
  }

  if (horizontal.empty()) return;
  // The floor is the lowest horizontal plane along `up`, the ceiling the
  // highest. Height is measured as the signed distance of the plane from the
  // origin along up, which for a plane with normal +-up is -offset * sign.
  const auto height = [&up](const DetectedPlane& p) {
    const double alignment = p.plane.normal.dot(up);
    return alignment >= 0.0 ? -p.plane.offset : p.plane.offset;
  };
  int lowest = horizontal.front();
  int highest = horizontal.front();
  for (int i : horizontal) {
    if (height((*planes)[i]) < height((*planes)[lowest])) lowest = i;
    if (height((*planes)[i]) > height((*planes)[highest])) highest = i;
  }
  for (int i : horizontal) (*planes)[i].role = PlaneRole::Unknown;
  (*planes)[lowest].role = PlaneRole::Floor;
  // Only call the top plane a ceiling when it is meaningfully above the floor;
  // a single tabletop scan has one horizontal plane and no ceiling.
  if (highest != lowest && height((*planes)[highest]) - height((*planes)[lowest]) > 1.2) {
    (*planes)[highest].role = PlaneRole::Ceiling;
  }
}

Mat3 levellingRotation(const std::vector<DetectedPlane>& planes,
                       const PlaneDetectionOptions& options) {
  const Vec3 up = options.up.normalized();
  for (const DetectedPlane& p : planes) {
    if (p.role != PlaneRole::Floor) continue;
    // Orient the floor normal upwards, then rotate it onto `up`.
    Vec3 normal = p.plane.normal;
    if (normal.dot(up) < 0.0) normal = -normal;
    const Vec3 axis = normal.cross(up);
    const double sine = axis.norm();
    const double cosine = normal.dot(up);
    if (sine < 1e-9) {
      // Already aligned, or anti-aligned (which the flip above ruled out).
      return Mat3::Identity();
    }
    const double angle = std::atan2(sine, cosine);
    return so3Exp(axis.normalized() * angle);
  }
  return Mat3::Identity();
}

}  // namespace parallax
