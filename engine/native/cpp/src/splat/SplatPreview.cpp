#include "parallax/splat/SplatPreview.h"

#include <algorithm>
#include <cmath>

namespace parallax {
namespace {

/// Any unit vector perpendicular to `normal`, chosen to stay well conditioned.
Vec3f perpendicularTo(const Vec3f& normal) {
  // Crossing with the world axis the normal is *least* aligned with keeps the
  // result far from degenerate; crossing with a fixed axis collapses whenever
  // the normal happens to point along it, which for a floor is always.
  const Vec3f axis = std::abs(normal.x()) < std::abs(normal.y())
                         ? (std::abs(normal.x()) < std::abs(normal.z()) ? Vec3f::UnitX()
                                                                        : Vec3f::UnitZ())
                         : (std::abs(normal.y()) < std::abs(normal.z()) ? Vec3f::UnitY()
                                                                        : Vec3f::UnitZ());
  const Vec3f result = normal.cross(axis);
  const float length = result.norm();
  return length > 1e-6f ? (result / length).eval() : Vec3f::UnitX();
}

/**
 * Median nearest-neighbour spacing, estimated from a sample.
 *
 * A disc radius has to come from how far apart the points are, and the mean is
 * no use for that: a scan always has a few outliers metres away from anything,
 * and they drag the mean up until every disc is the size of a dinner plate.
 */
float estimateSpacing(const std::vector<Vec3f>& points) {
  if (points.size() < 2) return 0.01f;
  const size_t sample_count = std::min<size_t>(points.size(), 512);
  const size_t step = std::max<size_t>(1, points.size() / sample_count);
  std::vector<float> distances;
  distances.reserve(sample_count);
  for (size_t i = 0; i < points.size(); i += step) {
    float nearest = std::numeric_limits<float>::max();
    // Against the same subsample rather than everything: this is a scale
    // estimate, and it only has to be right to within a factor of two.
    for (size_t j = 0; j < points.size(); j += step) {
      if (i == j) continue;
      nearest = std::min(nearest, (points[i] - points[j]).squaredNorm());
    }
    if (nearest < std::numeric_limits<float>::max()) distances.push_back(std::sqrt(nearest));
  }
  if (distances.empty()) return 0.01f;
  const size_t middle = distances.size() / 2;
  std::nth_element(distances.begin(), distances.begin() + middle, distances.end());
  return std::max(distances[middle], 1e-4f);
}

/// Append one n-gon fan as triangles, with a shared colour.
void appendDisc(Mesh* mesh, const Vec3f& centre, const Vec3f& normal, float radius, int segments,
                const std::array<uint8_t, 3>& color) {
  const Vec3f u = perpendicularTo(normal) * radius;
  const Vec3f v = normal.cross(u.normalized()) * radius;
  const uint32_t base = static_cast<uint32_t>(mesh->vertices.size());
  for (int s = 0; s < segments; ++s) {
    const float angle = 2.f * 3.14159265358979f * static_cast<float>(s) / segments;
    mesh->vertices.push_back(centre + u * std::cos(angle) + v * std::sin(angle));
    mesh->normals.push_back(normal);
    mesh->colors.push_back(color);
    mesh->uvs.emplace_back(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle));
  }
  for (int s = 1; s + 1 < segments; ++s) {
    mesh->indices.push_back(base);
    mesh->indices.push_back(base + static_cast<uint32_t>(s));
    mesh->indices.push_back(base + static_cast<uint32_t>(s) + 1);
  }
}

}  // namespace

Mesh buildPointCloudPreview(const std::vector<Vec3f>& points, const std::vector<Vec3f>& normals,
                            const std::vector<std::array<uint8_t, 3>>& colors,
                            const PointPreviewOptions& options) {
  Mesh mesh;
  if (points.empty()) return mesh;
  const int segments = std::clamp(options.segments, 3, 12);
  const int cap = std::max(1, options.max_points);

  // Even sampling rather than truncation: the first N points of a scan are
  // one corner of it, which is worse than a thinner version of the whole.
  const size_t step = points.size() > static_cast<size_t>(cap)
                          ? (points.size() + cap - 1) / cap
                          : 1;

  std::vector<Vec3f> kept;
  kept.reserve(points.size() / step + 1);
  for (size_t i = 0; i < points.size(); i += step) kept.push_back(points[i]);

  float radius = options.radius_metres;
  if (radius <= 0.f) {
    // Slightly over half the spacing, so neighbouring discs just touch and
    // the surface reads as continuous without swelling.
    radius = 0.62f * estimateSpacing(kept);
  }

  mesh.vertices.reserve(kept.size() * segments);
  mesh.indices.reserve(kept.size() * (segments - 2) * 3);
  for (size_t i = 0; i < points.size(); i += step) {
    Vec3f normal = i < normals.size() ? normals[i] : Vec3f::UnitY();
    const float length = normal.norm();
    normal = length > 1e-6f ? (normal / length).eval() : Vec3f::UnitY();
    const std::array<uint8_t, 3> color =
        i < colors.size() ? colors[i] : std::array<uint8_t, 3>{200, 200, 200};
    appendDisc(&mesh, points[i], normal, radius, segments, color);
  }
  return mesh;
}

Mesh buildSplatPreview(const GaussianCloud& cloud, const SplatPreviewOptions& options) {
  Mesh mesh;
  if (cloud.empty()) return mesh;

  // Sorted by opacity so that a truncated preview keeps what carries the
  // image, and so the draw order is at least broadly back-to-front by weight.
  GaussianCloud working = cloud;
  working.sortByOpacity();
  const size_t cap = static_cast<size_t>(std::max(1, options.max_gaussians));
  const float extent = std::max(options.extent_sigma, 0.5f);

  mesh.vertices.reserve(std::min(working.size(), cap) * 4);
  mesh.indices.reserve(std::min(working.size(), cap) * 6);

  for (const Gaussian& gaussian : working.gaussians) {
    if (mesh.vertices.size() / 4 >= cap) break;
    const float opacity = gaussianOpacity(gaussian);
    if (opacity < options.min_opacity) continue;

    const Vec3f scale = gaussian.log_scale.array().exp();
    const Eigen::Matrix3f rotation = gaussian.rotation.normalized().toRotationMatrix();
    // Drop the thinnest axis: a gaussian is a flattened ellipsoid and what is
    // visible is its widest cross-section, which is the same approximation the
    // real rasteriser's projected covariance makes.
    int thinnest = 0;
    for (int a = 1; a < 3; ++a) {
      if (scale[a] < scale[thinnest]) thinnest = a;
    }
    const int first = (thinnest + 1) % 3;
    const int second = (thinnest + 2) % 3;
    const Vec3f u = rotation.col(first) * (scale[first] * extent);
    const Vec3f v = rotation.col(second) * (scale[second] * extent);
    const Vec3f normal = rotation.col(thinnest);

    const std::array<float, 3> linear = gaussianBaseColor(gaussian);
    const std::array<uint8_t, 3> color{
        static_cast<uint8_t>(std::lround(std::clamp(linear[0], 0.f, 1.f) * 255.f)),
        static_cast<uint8_t>(std::lround(std::clamp(linear[1], 0.f, 1.f) * 255.f)),
        static_cast<uint8_t>(std::lround(std::clamp(linear[2], 0.f, 1.f) * 255.f))};

    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const float corners[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
    for (const auto& corner : corners) {
      mesh.vertices.push_back(gaussian.position + u * corner[0] + v * corner[1]);
      mesh.normals.push_back(normal);
      mesh.colors.push_back(color);
      mesh.uvs.emplace_back(0.5f + 0.5f * corner[0], 0.5f + 0.5f * corner[1]);
    }
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
  }
  return mesh;
}

ColorBuffer splatFalloffTexture(int size, float extent_sigma) {
  const int side = std::clamp(size, 4, 1024);
  const float extent = std::max(extent_sigma, 0.5f);
  ColorBuffer image(side, side);
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      // Pixel centres, mapped onto [-extent, extent] in both axes.
      const float u = (2.f * (static_cast<float>(x) + 0.5f) / side - 1.f) * extent;
      const float v = (2.f * (static_cast<float>(y) + 0.5f) / side - 1.f) * extent;
      const float falloff = std::exp(-0.5f * (u * u + v * v));
      const auto value = static_cast<uint8_t>(std::lround(std::clamp(falloff, 0.f, 1.f) * 255.f));
      image.set(x, y, value, value, value);
    }
  }
  return image;
}

}  // namespace parallax
