#include "parallax/testing/SyntheticScene.h"

#include <algorithm>
#include <cmath>

namespace parallax {
namespace testing {
namespace {

/// Smooth value noise in [0,1], built from a hashed integer lattice. Cheap and
/// deterministic across platforms (no floating point in the hash).
double valueNoise(double x, double y) {
  auto hash = [](int64_t xi, int64_t yi) {
    uint64_t h = static_cast<uint64_t>(xi) * 0x9E3779B97F4A7C15ull ^
                 static_cast<uint64_t>(yi) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (h >> 11) * (1.0 / 9007199254740992.0);
  };
  const double fx = std::floor(x), fy = std::floor(y);
  const int64_t xi = static_cast<int64_t>(fx), yi = static_cast<int64_t>(fy);
  const double tx = x - fx, ty = y - fy;
  // Quintic smoothstep for C2 continuity.
  const double sx = tx * tx * tx * (tx * (tx * 6 - 15) + 10);
  const double sy = ty * ty * ty * (ty * (ty * 6 - 15) + 10);
  const double v00 = hash(xi, yi), v10 = hash(xi + 1, yi);
  const double v01 = hash(xi, yi + 1), v11 = hash(xi + 1, yi + 1);
  return (v00 * (1 - sx) + v10 * sx) * (1 - sy) + (v01 * (1 - sx) + v11 * sx) * sy;
}

/// Evaluate a procedural texture at surface coordinates (u,v), returning a
/// multiplier in roughly [0.15, 1.0].
double textureValue(Texture texture, double u, double v, double scale) {
  switch (texture) {
    case Texture::Checker: {
      const int cu = static_cast<int>(std::floor(u * scale));
      const int cv = static_cast<int>(std::floor(v * scale));
      return ((cu + cv) & 1) ? 0.95 : 0.22;
    }
    case Texture::Noise: {
      // Two octaves: broad structure plus fine speckle, so corner detectors
      // have something to lock onto at several pyramid levels.
      const double n = 0.65 * valueNoise(u * scale, v * scale) +
                       0.35 * valueNoise(u * scale * 3.7 + 11.0, v * scale * 3.7 - 5.0);
      return 0.18 + 0.78 * n;
    }
    case Texture::Stripes: {
      const int cu = static_cast<int>(std::floor(u * scale));
      return (cu & 1) ? 0.9 : 0.25;
    }
    case Texture::Flat:
    default:
      return 0.65;
  }
}

/// Surface parameterisation for texturing: picks the two axes least aligned
/// with the normal so the mapping never degenerates.
void surfaceUv(const Vec3& point, const Vec3& normal, double* u, double* v) {
  const Vec3 n = normal.cwiseAbs();
  if (n.x() >= n.y() && n.x() >= n.z()) {
    *u = point.y();
    *v = point.z();
  } else if (n.y() >= n.z()) {
    *u = point.x();
    *v = point.z();
  } else {
    *u = point.x();
    *v = point.y();
  }
}

/// Ray/sphere intersection, nearest positive root.
double intersectSphere(const Vec3& o, const Vec3& d, const Vec3& c, double r) {
  const Vec3 oc = o - c;
  const double b = oc.dot(d);
  const double disc = b * b - (oc.squaredNorm() - r * r);
  if (disc < 0.0) return -1.0;
  const double sq = std::sqrt(disc);
  const double t0 = -b - sq;
  if (t0 > 1e-6) return t0;
  const double t1 = -b + sq;
  return t1 > 1e-6 ? t1 : -1.0;
}

/// Ray/axis-aligned-box via the slab method; also reports the hit normal.
double intersectBox(const Vec3& o, const Vec3& d, const Vec3& c, const Vec3& h, Vec3* normal) {
  double tmin = -1e30, tmax = 1e30;
  int axis = 0;
  double sign = 1.0;
  for (int i = 0; i < 3; ++i) {
    if (std::abs(d[i]) < 1e-12) {
      if (std::abs(o[i] - c[i]) > h[i]) return -1.0;
      continue;
    }
    const double inv = 1.0 / d[i];
    double t1 = (c[i] - h[i] - o[i]) * inv;
    double t2 = (c[i] + h[i] - o[i]) * inv;
    double s = -1.0;
    if (t1 > t2) { std::swap(t1, t2); s = 1.0; }
    if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return -1.0;
  }
  const double t = tmin > 1e-6 ? tmin : tmax;
  if (t <= 1e-6) return -1.0;
  if (tmin > 1e-6) {
    *normal = Vec3::Zero();
    (*normal)[axis] = sign;
  } else {
    // Ray started inside the box; the normal is unused in that case but must
    // still be finite.
    *normal = -d.normalized();
  }
  return t;
}

/// Ray/plane intersection for an infinite plane through `c` with normal `n`.
double intersectPlane(const Vec3& o, const Vec3& d, const Vec3& c, const Vec3& n) {
  const double denom = d.dot(n);
  if (std::abs(denom) < 1e-12) return -1.0;
  const double t = (c - o).dot(n) / denom;
  return t > 1e-6 ? t : -1.0;
}

}  // namespace

CameraIntrinsics phoneIntrinsics(int width, int height, double horizontal_fov_deg) {
  CameraIntrinsics k;
  k.width = width;
  k.height = height;
  k.fx = (width * 0.5) / std::tan(deg2rad(horizontal_fov_deg) * 0.5);
  k.fy = k.fx;  // square pixels, as on essentially every phone sensor
  k.cx = width * 0.5 - 0.5;
  k.cy = height * 0.5 - 0.5;
  return k;
}

double SyntheticScene::castRay(const Vec3& origin, const Vec3& direction, Vec3* out_normal,
                               Vec3* out_albedo) const {
  const Vec3 d = direction.normalized();
  double best_t = -1.0;
  Vec3 best_n = Vec3::UnitY();
  const Primitive* best_prim = nullptr;

  for (const Primitive& prim : primitives) {
    double t = -1.0;
    Vec3 n = Vec3::UnitY();
    switch (prim.shape) {
      case Shape::Sphere:
        t = intersectSphere(origin, d, prim.center, prim.radius);
        if (t > 0.0) n = (origin + d * t - prim.center).normalized();
        break;
      case Shape::Box:
        t = intersectBox(origin, d, prim.center, prim.half_extent, &n);
        break;
      case Shape::Plane:
        t = intersectPlane(origin, d, prim.center, prim.normal.normalized());
        if (t > 0.0) {
          n = prim.normal.normalized();
          // Face the viewer so shading is well defined from both sides.
          if (n.dot(d) > 0.0) n = -n;
        }
        break;
    }
    if (t > 1e-6 && (best_t < 0.0 || t < best_t)) {
      best_t = t;
      best_n = n;
      best_prim = &prim;
    }
  }
  if (best_t < 0.0) return -1.0;

  if (out_normal) *out_normal = best_n;
  if (out_albedo) {
    const Vec3 hit = origin + d * best_t;
    double u = 0.0, v = 0.0;
    surfaceUv(hit, best_n, &u, &v);
    const double tex = textureValue(best_prim->texture, u, v, best_prim->texture_scale);
    *out_albedo = best_prim->albedo * tex;
  }
  return best_t;
}

RenderResult SyntheticScene::render(const CameraIntrinsics& intrinsics, const Pose& pose,
                                    bool want_color, bool want_depth) const {
  RenderResult out;
  out.gray = GrayBuffer(intrinsics.width, intrinsics.height);
  if (want_color) out.color = ColorBuffer(intrinsics.width, intrinsics.height);
  if (want_depth) out.depth = DepthMap(intrinsics.width, intrinsics.height);

  // `pose` is camera-from-world, so the inverse gives us the camera centre and
  // the rotation that takes camera-space rays into world space.
  const Pose cam_to_world = pose.inverse();
  const Vec3 eye = cam_to_world.t;
  const Mat3 R_wc = cam_to_world.R;

  for (int y = 0; y < intrinsics.height; ++y) {
    for (int x = 0; x < intrinsics.width; ++x) {
      const Vec3 ray_cam = intrinsics.unproject(Vec2(x, y));
      const Vec3 ray_world = (R_wc * ray_cam).normalized();

      Vec3 normal, albedo;
      const double t = castRay(eye, ray_world, &normal, &albedo);
      if (t < 0.0) {
        out.gray.at(x, y) = 0;
        continue;
      }
      // Lambertian shading with a constant ambient term. No shadows: the goal
      // is repeatable texture detail, not photorealism.
      const double lambert = std::max(0.0, -normal.dot(light_direction));
      const double shade = std::min(1.0, ambient + (1.0 - ambient) * lambert);
      const Vec3 rgb = (albedo * shade * 255.0).cwiseMin(255.0).cwiseMax(0.0);
      // Rec.601 luma, matching what Android's YUV_420_888 Y plane carries.
      const double luma = 0.299 * rgb.x() + 0.587 * rgb.y() + 0.114 * rgb.z();
      out.gray.at(x, y) = static_cast<uint8_t>(std::lround(luma));
      if (want_color) {
        out.color.set(x, y, static_cast<uint8_t>(std::lround(rgb.x())),
                      static_cast<uint8_t>(std::lround(rgb.y())),
                      static_cast<uint8_t>(std::lround(rgb.z())));
      }
      if (want_depth) {
        // Store depth along the optical axis (z in camera space), which is what
        // ARCore's Depth API and every TSDF integrator expect -- not the ray
        // length t.
        const Vec3 p_cam = pose * (eye + ray_world * t);
        out.depth.at(x, y) = static_cast<float>(p_cam.z());
      }
    }
  }
  return out;
}

Pose SyntheticScene::lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
  // Build the camera-from-world rotation directly. Camera convention: +X right,
  // +Y down, +Z forward -- the OpenCV image convention, which is the engine's throughout.
  // Not ARCore's: its camera pose is +Y up and -Z forward, and the two differ by a half turn
  // about X. This comment used to claim they were the same, and every device scan fused its
  // frames behind the camera. The turn is applied where the ARCore pose is read.
  Vec3 forward = (target - eye);
  if (forward.norm() < 1e-9) forward = Vec3::UnitZ();
  forward.normalize();
  Vec3 world_up = up;
  if (std::abs(world_up.normalized().dot(forward)) > 0.999) {
    // Degenerate: looking straight along `up`. Pick any perpendicular axis.
    world_up = std::abs(forward.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitZ();
  }
  Vec3 right = forward.cross(world_up.normalized());
  if (right.norm() < 1e-9) right = Vec3::UnitX();
  right.normalize();
  const Vec3 down = forward.cross(right);

  Mat3 R;  // rows are the camera axes expressed in world coordinates
  R.row(0) = right.transpose();
  R.row(1) = down.transpose();
  R.row(2) = forward.transpose();
  return Pose(orthonormalize(R), -R * eye);
}

std::vector<Pose> SyntheticScene::orbitPoses(const Vec3& target, double radius, double elevation,
                                             int count, double start_angle, double sweep) {
  std::vector<Pose> poses;
  poses.reserve(std::max(0, count));
  if (count <= 0) return poses;
  // When sweeping a full circle the last sample would duplicate the first, so
  // the step divides by `count`; for partial sweeps we include both endpoints.
  const bool closed = std::abs(sweep - 2.0 * kPi) < 1e-9;
  const double step = closed ? sweep / count : (count > 1 ? sweep / (count - 1) : 0.0);
  for (int i = 0; i < count; ++i) {
    const double a = start_angle + step * i;
    const Vec3 eye = target + Vec3(radius * std::cos(a), elevation, radius * std::sin(a));
    poses.push_back(lookAt(eye, target));
  }
  return poses;
}

SyntheticScene SyntheticScene::objectOnTable() {
  SyntheticScene scene;
  // Ground plane at y = 0, noise textured so the floor is trackable.
  Primitive floor;
  floor.shape = Shape::Plane;
  floor.center = Vec3(0, 0, 0);
  floor.normal = Vec3::UnitY();
  floor.texture = Texture::Noise;
  floor.texture_scale = 6.0;
  floor.albedo = Vec3(0.55, 0.55, 0.58);
  scene.primitives.push_back(floor);

  // The subject: a box with a checkerboard, plus a sphere resting on it, which
  // together give both planar and curved surfaces to reconstruct.
  Primitive box;
  box.shape = Shape::Box;
  box.center = Vec3(0, 0.18, 0);
  box.half_extent = Vec3(0.22, 0.18, 0.16);
  box.texture = Texture::Checker;
  box.texture_scale = 14.0;
  box.albedo = Vec3(0.85, 0.80, 0.72);
  scene.primitives.push_back(box);

  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.center = Vec3(0.0, 0.47, 0.0);
  ball.radius = 0.11;
  ball.texture = Texture::Noise;
  ball.texture_scale = 22.0;
  ball.albedo = Vec3(0.80, 0.45, 0.35);
  scene.primitives.push_back(ball);

  return scene;
}

SyntheticScene SyntheticScene::room(double width, double depth, double height) {
  SyntheticScene scene;
  scene.ambient = 0.45;
  const double hw = width * 0.5, hd = depth * 0.5;

  auto addPlane = [&scene](const Vec3& c, const Vec3& n, Texture tex, double scale,
                           const Vec3& albedo) {
    Primitive p;
    p.shape = Shape::Plane;
    p.center = c;
    p.normal = n;
    p.texture = tex;
    p.texture_scale = scale;
    p.albedo = albedo;
    scene.primitives.push_back(p);
  };
  // Walls are modelled as infinite planes; since every camera sits inside the
  // box, each plane is only ever seen from one side.
  addPlane(Vec3(0, 0, 0), Vec3::UnitY(), Texture::Noise, 3.0, Vec3(0.45, 0.42, 0.40));
  addPlane(Vec3(0, height, 0), -Vec3::UnitY(), Texture::Flat, 1.0, Vec3(0.92, 0.92, 0.92));
  addPlane(Vec3(-hw, 0, 0), Vec3::UnitX(), Texture::Noise, 2.4, Vec3(0.78, 0.76, 0.72));
  addPlane(Vec3(hw, 0, 0), -Vec3::UnitX(), Texture::Noise, 2.4, Vec3(0.78, 0.76, 0.72));
  addPlane(Vec3(0, 0, -hd), Vec3::UnitZ(), Texture::Noise, 2.4, Vec3(0.80, 0.78, 0.74));
  addPlane(Vec3(0, 0, hd), -Vec3::UnitZ(), Texture::Noise, 2.4, Vec3(0.80, 0.78, 0.74));
  return scene;
}

std::vector<Vec3> SyntheticScene::samplePoints(int count, uint64_t seed) const {
  // Rejection sampling: fire rays inward from a bounding sphere and keep the
  // first hit. This gives a roughly area-uniform sample of the visible surface
  // without needing per-primitive parameterisations.
  std::vector<Vec3> points;
  if (primitives.empty() || count <= 0) return points;
  points.reserve(count);
  Rng rng(seed);

  // Bound the interesting region from the non-plane primitives, falling back to
  // a fixed radius when the scene is all planes (as the room scene is).
  Vec3 centroid = Vec3::Zero();
  double radius = 0.0;
  int finite = 0;
  for (const Primitive& p : primitives) {
    if (p.shape == Shape::Plane) continue;
    centroid += p.center;
    ++finite;
  }
  if (finite > 0) {
    centroid /= finite;
    for (const Primitive& p : primitives) {
      if (p.shape == Shape::Plane) continue;
      const double extent = p.shape == Shape::Sphere ? p.radius : p.half_extent.norm();
      radius = std::max(radius, (p.center - centroid).norm() + extent);
    }
    radius = std::max(radius * 2.5, 0.5);
  } else {
    radius = 6.0;
  }

  int guard = 0;
  while (static_cast<int>(points.size()) < count && guard++ < count * 200) {
    const Vec3 dir = rng.unitVector();
    const Vec3 origin = centroid + dir * radius;
    // Aim back at a random point near the centroid so rays actually hit.
    const Vec3 aim = centroid + rng.unitVector() * rng.uniform(0.0, radius * 0.25);
    const double t = castRay(origin, (aim - origin).normalized(), nullptr, nullptr);
    if (t > 0.0) points.push_back(origin + (aim - origin).normalized() * t);
  }
  return points;
}

}  // namespace testing
}  // namespace parallax
