// Synthetic capture generator: ground-truth datasets for regression testing.
//
// A scene is a small set of analytic primitives (planes, boxes, spheres) with
// procedural textures. Rendering is done by exact ray casting, so every frame
// comes with a pixel-accurate depth map and an exactly known camera pose. That
// gives the reconstruction tests a ground truth to measure against instead of
// the usual "does it crash" smoke test.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"

namespace parallax {
namespace testing {

enum class Texture {
  Checker,      ///< high-contrast checkerboard, the easy case for features
  Noise,        ///< value-noise speckle, closer to real surface detail
  Stripes,      ///< one-dimensional detail; degenerate for some estimators
  Flat,         ///< untextured; used to test low-texture failure handling
};

enum class Shape { Plane, Box, Sphere };

struct Primitive {
  Shape shape = Shape::Sphere;
  Vec3 center = Vec3::Zero();
  Vec3 half_extent = Vec3(0.5, 0.5, 0.5);  ///< Box only
  Vec3 normal = Vec3::UnitY();             ///< Plane only
  double radius = 0.5;                     ///< Sphere only
  Texture texture = Texture::Checker;
  double texture_scale = 8.0;
  Vec3 albedo = Vec3(0.8, 0.8, 0.8);
};

struct RenderResult {
  GrayBuffer gray;
  ColorBuffer color;
  DepthMap depth;      ///< exact ray-cast depth in metres, 0 where nothing hit
};

/// A scene plus the cameras that observe it.
class SyntheticScene {
 public:
  std::vector<Primitive> primitives;
  Vec3 light_direction = Vec3(-0.4, -1.0, -0.35).normalized();
  double ambient = 0.35;

  /// An object on a ground plane, viewed from an orbit. Mirrors "object mode".
  static SyntheticScene objectOnTable();
  /// A closed room with textured walls, floor and ceiling. Mirrors "room mode".
  static SyntheticScene room(double width = 4.0, double depth = 3.0, double height = 2.5);

  /// Camera poses on a circular orbit around `target` at the given radius and
  /// elevation, all looking at the target.
  static std::vector<Pose> orbitPoses(const Vec3& target, double radius, double elevation,
                                      int count, double start_angle = 0.0,
                                      double sweep = 2.0 * kPi);

  /// A camera pose that looks from `eye` towards `target` with +Y roughly up.
  static Pose lookAt(const Vec3& eye, const Vec3& target, const Vec3& up = Vec3(0, 1, 0));

  /// Render one view. `color` and `depth` are filled only if requested.
  RenderResult render(const CameraIntrinsics& intrinsics, const Pose& pose,
                      bool want_color = true, bool want_depth = true) const;

  /// Cast a single ray; returns distance along the ray or -1 if it misses.
  /// `out_normal` and `out_albedo` describe the surface hit.
  double castRay(const Vec3& origin, const Vec3& direction, Vec3* out_normal,
                 Vec3* out_albedo) const;

  /// Ground-truth 3D points sampled uniformly over the visible surfaces. Used
  /// to score reconstructed meshes and point clouds.
  std::vector<Vec3> samplePoints(int count, uint64_t seed = 7) const;
};

/// Deterministic, platform-independent uniform generator used by the scene and
/// by the noise textures (std::mt19937 output is portable but the
/// distributions built on it are not).
class Rng {
 public:
  explicit Rng(uint64_t seed = 88172645463325252ull) : state_(seed ? seed : 1) {}
  uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  /// Uniform in [0,1).
  double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  /// Standard normal via Box-Muller.
  double normal() {
    const double u1 = std::max(1e-12, uniform());
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
  }
  Vec3 unitVector() {
    const double z = uniform(-1.0, 1.0);
    const double a = uniform(0.0, 2.0 * kPi);
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return Vec3(r * std::cos(a), r * std::sin(a), z);
  }

 private:
  uint64_t state_;
};

/// Default intrinsics for a typical phone main camera at the given resolution.
CameraIntrinsics phoneIntrinsics(int width = 640, int height = 480,
                                 double horizontal_fov_deg = 64.0);

}  // namespace testing
}  // namespace parallax
