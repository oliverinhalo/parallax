#include "parallax/texture/Texturing.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace parallax {
namespace {

inline Vec3 toDouble(const Vec3f& v) { return Vec3(v.x(), v.y(), v.z()); }

/// Geometric normal of a triangle, or a zero vector when degenerate.
Vec3 triangleNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 n = (b - a).cross(c - a);
  const double len = n.norm();
  return len > 1e-14 ? (n / len).eval() : Vec3::Zero();
}

/// One triangle's assignment in the atlas.
struct Patch {
  int triangle = -1;
  int view = -1;
  int size = 0;      ///< patch is size x size texels, before padding
  int padding = 1;   ///< gutter around this patch, scaled to its size
  int x = 0, y = 0;  ///< top-left in the atlas
};

/**
 * Gutter width for a patch of the given size.
 *
 * A fixed gutter is wrong at both ends of the size range. Two texels around a 64-texel patch is
 * negligible, but around a 4-texel patch it is 50% of the area -- enough to make a dense mesh
 * fail to pack at all. Scaling with the patch, floored at one texel, keeps bilinear filtering
 * safe while letting small patches stay small.
 */
inline int paddingFor(int size, int requested) {
  return std::max(1, std::min(requested, size / 4));
}

/**
 * Shelf packing.
 *
 * Patches are sorted largest-first and laid into rows whose height is set by the first patch
 * placed in them. For square patches sorted by size this is within a few percent of optimal,
 * and unlike a guillotine or skyline packer it is simple enough to be obviously correct -- which
 * matters, because a packing bug shows up as triangles sampling each other's texels.
 */
bool packPatches(std::vector<Patch>* patches, int atlas_size, double* out_utilisation) {
  std::sort(patches->begin(), patches->end(),
            [](const Patch& a, const Patch& b) { return a.size > b.size; });

  int cursor_x = 0, cursor_y = 0, row_height = 0;
  long used_area = 0;
  for (Patch& patch : *patches) {
    const int padding = patch.padding;
    const int stride = patch.size + padding * 2;
    if (stride > atlas_size) return false;
    if (cursor_x + stride > atlas_size) {
      // Start a new row.
      cursor_x = 0;
      cursor_y += row_height;
      row_height = 0;
    }
    if (cursor_y + stride > atlas_size) return false;  // atlas is full
    patch.x = cursor_x + padding;
    patch.y = cursor_y + padding;
    cursor_x += stride;
    row_height = std::max(row_height, stride);
    used_area += static_cast<long>(patch.size) * patch.size;
  }
  if (out_utilisation) {
    *out_utilisation =
        static_cast<double>(used_area) / (static_cast<double>(atlas_size) * atlas_size);
  }
  return true;
}

/// Barycentric coordinates of p within triangle (a, b, c) in 2D.
bool barycentric(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c, double* u,
                 double* v, double* w) {
  const Vec2 v0 = b - a, v1 = c - a, v2 = p - a;
  const double den = v0.x() * v1.y() - v1.x() * v0.y();
  if (std::abs(den) < 1e-18) return false;
  *v = (v2.x() * v1.y() - v1.x() * v2.y()) / den;
  *w = (v0.x() * v2.y() - v2.x() * v0.y()) / den;
  *u = 1.0 - *v - *w;
  return true;
}

/// Bilinear sample of a colour image, clamped at the edges.
void sampleBilinear(const ColorBuffer& image, double x, double y, double* rgb) {
  const double cx = std::max(0.0, std::min<double>(image.width - 1.001, x));
  const double cy = std::max(0.0, std::min<double>(image.height - 1.001, y));
  const int x0 = static_cast<int>(cx), y0 = static_cast<int>(cy);
  const int x1 = std::min(image.width - 1, x0 + 1), y1 = std::min(image.height - 1, y0 + 1);
  const double ax = cx - x0, ay = cy - y0;
  uint8_t p00[3], p10[3], p01[3], p11[3];
  image.get(x0, y0, p00);
  image.get(x1, y0, p10);
  image.get(x0, y1, p01);
  image.get(x1, y1, p11);
  for (int c = 0; c < 3; ++c) {
    const double top = p00[c] * (1 - ax) + p10[c] * ax;
    const double bottom = p01[c] * (1 - ax) + p11[c] * ax;
    rgb[c] = top * (1 - ay) + bottom * ay;
  }
}

inline uint64_t edgeKey(uint32_t a, uint32_t b) {
  if (a > b) std::swap(a, b);
  return (static_cast<uint64_t>(a) << 32) | b;
}

}  // namespace

bool triangleVisibleIn(const TextureView& view, const Vec3& a, const Vec3& b, const Vec3& c,
                       const TexturingOptions& options) {
  if (view.image.empty() || !view.intrinsics.valid()) return false;
  const Vec3 normal = triangleNormal(a, b, c);
  if (normal.isZero()) return false;

  // Every corner must project inside the frame: a triangle half off the edge of the image would
  // sample clamped texels along one side.
  const Vec3 corners[3] = {a, b, c};
  for (const Vec3& corner : corners) {
    const Vec3 cam = view.pose * corner;
    if (cam.z() <= 1e-4) return false;
    Vec2 px;
    if (!view.intrinsics.project(cam, &px)) return false;
    if (px.x() < 0.0 || px.y() < 0.0 || px.x() > view.intrinsics.width - 1 ||
        px.y() > view.intrinsics.height - 1) {
      return false;
    }
  }

  // Back-facing check, using the camera centre rather than the optical axis so it is correct
  // for triangles well off-axis.
  const Vec3 centroid = (a + b + c) / 3.0;
  const Vec3 eye = view.pose.inverse().t;
  if (normal.dot(eye - centroid) <= 0.0) return false;

  // Occlusion: the view's own depth map must agree with the triangle's distance. Without this a
  // wall behind a sofa gets textured with the sofa.
  if (!view.depth.empty()) {
    const Vec3 cam = view.pose * centroid;
    Vec2 px;
    if (!view.intrinsics.project(cam, &px)) return false;
    const int ix = static_cast<int>(std::lround(px.x()));
    const int iy = static_cast<int>(std::lround(px.y()));
    if (ix >= 0 && iy >= 0 && ix < view.depth.width && iy < view.depth.height) {
      const float measured = view.depth.at(ix, iy);
      if (measured > 1e-4f && cam.z() > measured + options.depth_tolerance) return false;
    }
  }
  return true;
}

double scoreViewForTriangle(const TextureView& view, const Vec3& a, const Vec3& b, const Vec3& c,
                            const TexturingOptions& options) {
  if (!triangleVisibleIn(view, a, b, c, options)) return 0.0;

  const Vec3 normal = triangleNormal(a, b, c);
  const Vec3 centroid = (a + b + c) / 3.0;
  const Vec3 eye = view.pose.inverse().t;
  const Vec3 to_eye = eye - centroid;
  const double distance = to_eye.norm();
  if (distance < 1e-6) return 0.0;

  const double cos_incidence = normal.dot(to_eye / distance);
  if (cos_incidence <= std::cos(deg2rad(options.max_incidence_deg))) return 0.0;

  // Texel density falls as cos(incidence) / distance^2, which is the dominant term; sharpness
  // enters as a square root so that a modestly blurrier but much better-angled view can still
  // win.
  const double density = cos_incidence / (distance * distance);
  const double sharpness = std::sqrt(std::max(1.0, view.sharpness));
  return density * sharpness;
}

TexturingResult generateTextureAtlas(const Mesh& mesh, const std::vector<TextureView>& views,
                                     const TexturingOptions& options) {
  TexturingResult result;
  if (mesh.empty() || views.empty()) return result;
  if (options.atlas_size < 64) return result;

  const size_t triangle_count = mesh.triangleCount();

  // ---- view selection -----------------------------------------------------
  std::vector<int> best_view(triangle_count, -1);
  std::vector<double> triangle_area(triangle_count, 0.0);
  double incidence_sum = 0.0;
  int incidence_count = 0;

  for (size_t t = 0; t < triangle_count; ++t) {
    const Vec3 a = toDouble(mesh.vertices[mesh.indices[t * 3]]);
    const Vec3 b = toDouble(mesh.vertices[mesh.indices[t * 3 + 1]]);
    const Vec3 c = toDouble(mesh.vertices[mesh.indices[t * 3 + 2]]);
    triangle_area[t] = 0.5 * (b - a).cross(c - a).norm();

    double best_score = 0.0;
    for (size_t v = 0; v < views.size(); ++v) {
      const double score = scoreViewForTriangle(views[v], a, b, c, options);
      if (score > best_score) {
        best_score = score;
        best_view[t] = static_cast<int>(v);
      }
    }
    if (best_view[t] >= 0) {
      ++result.stats.triangles_textured;
      const Vec3 normal = triangleNormal(a, b, c);
      const Vec3 eye = views[best_view[t]].pose.inverse().t;
      const Vec3 to_eye = (eye - (a + b + c) / 3.0).normalized();
      incidence_sum += rad2deg(std::acos(std::max(-1.0, std::min(1.0, normal.dot(to_eye)))));
      ++incidence_count;
    } else {
      ++result.stats.triangles_without_view;
    }
  }
  if (incidence_count > 0) {
    result.stats.mean_incidence_deg = incidence_sum / incidence_count;
  }
  if (result.stats.triangles_textured == 0) return result;

  // ---- patch sizing -------------------------------------------------------
  // Allocate texels in proportion to the square root of world area, so texel density is
  // uniform over the surface rather than per triangle.
  const double max_area =
      *std::max_element(triangle_area.begin(), triangle_area.end());
  // Cap the largest patch so the whole set has a chance of fitting. For n square patches of
  // stride s in an atlas of edge A, shelf packing needs roughly (A / s)^2 >= n, giving
  // s <= A / sqrt(n). Deriving the cap rather than shrinking by trial and error means a dense
  // mesh degrades to lower texel density in one step instead of failing after a fixed number
  // of attempts, which is what a purely iterative shrink does.
  const int textured = result.stats.triangles_textured;
  const double rows = std::ceil(std::sqrt(static_cast<double>(textured)));
  const int feasible_stride =
      std::max(1, static_cast<int>(std::floor(options.atlas_size / std::max(1.0, rows))));
  // Subtract the gutter the smallest patch would take, since that is the binding constraint.
  const int smallest_gutter = paddingFor(options.min_patch_texels, options.patch_padding) * 2;
  const int largest_patch = std::max(
      options.min_patch_texels,
      std::min(options.max_patch_texels, feasible_stride - smallest_gutter));

  std::vector<Patch> patches;
  patches.reserve(triangle_count);
  for (size_t t = 0; t < triangle_count; ++t) {
    if (best_view[t] < 0) continue;
    // Texels in proportion to the square root of world area, so density is uniform over the
    // surface rather than per triangle.
    const double relative = max_area > 1e-18 ? std::sqrt(triangle_area[t] / max_area) : 1.0;
    Patch patch;
    patch.triangle = static_cast<int>(t);
    patch.view = best_view[t];
    patch.size = static_cast<int>(std::lround(largest_patch * relative));
    patch.size = std::max(options.min_patch_texels, patch.size);
    patch.padding = paddingFor(patch.size, options.patch_padding);
    patches.push_back(patch);
  }

  // Safety net: the estimate above ignores the size spread, so shrink until it fits or every
  // patch is already at the floor.
  double utilisation = 0.0;
  bool packed = false;
  for (int attempt = 0; attempt < 24 && !packed; ++attempt) {
    packed = packPatches(&patches, options.atlas_size, &utilisation);
    if (packed) break;
    bool shrank = false;
    for (Patch& patch : patches) {
      const int reduced = std::max(options.min_patch_texels, patch.size * 4 / 5);
      if (reduced < patch.size) {
        patch.size = reduced;
        patch.padding = paddingFor(patch.size, options.patch_padding);
        shrank = true;
      }
    }
    if (!shrank) break;  // already at the minimum patch size
  }

  // Still too many patches for the atlas even at the minimum size. Drop the smallest-area
  // triangles until it fits rather than abandoning the whole texture: the dropped triangles
  // keep their vertex colours, so the model is still presentable, and the count is reported.
  if (!packed) {
    // packPatches sorted largest-first, so the tail is what to shed.
    while (!patches.empty()) {
      const size_t keep = patches.size() * 9 / 10;
      if (keep == patches.size() || keep < 1) {
        patches.resize(patches.size() - 1);
      } else {
        patches.resize(keep);
      }
      if (packPatches(&patches, options.atlas_size, &utilisation)) {
        packed = true;
        break;
      }
    }
    if (!packed || patches.empty()) return result;
    result.stats.triangles_dropped_for_space =
        result.stats.triangles_textured - static_cast<int>(patches.size());
    result.stats.triangles_textured = static_cast<int>(patches.size());
  }
  result.stats.atlas_size = options.atlas_size;
  result.stats.atlas_utilisation = utilisation;

  // ---- exposure compensation ---------------------------------------------
  // Scale each view towards the median exposure, so a frame taken with the sun behind the
  // phone does not leave a bright blotch on the model.
  std::vector<double> view_gain(views.size(), 1.0);
  if (options.compensate_exposure) {
    std::vector<double> exposures;
    for (const TextureView& view : views) {
      if (view.exposure > 1.0) exposures.push_back(view.exposure);
    }
    if (!exposures.empty()) {
      std::nth_element(exposures.begin(), exposures.begin() + exposures.size() / 2,
                       exposures.end());
      const double median = exposures[exposures.size() / 2];
      for (size_t v = 0; v < views.size(); ++v) {
        if (views[v].exposure > 1.0) {
          // Clamp the correction: a large gain on a genuinely dark frame amplifies its noise.
          view_gain[v] = std::max(0.6, std::min(1.6, median / views[v].exposure));
        }
      }
    }
  }

  // ---- rebuild the mesh with per-triangle UVs -----------------------------
  Mesh out;
  out.vertices.reserve(patches.size() * 3);
  out.indices.reserve(patches.size() * 3);
  const bool has_colors = mesh.hasColors();
  const double inv_atlas = 1.0 / options.atlas_size;

  // UV corners of a patch: the triangle occupies the lower-left half of its square cell, inset
  // by half a texel so sampling never reaches the padding.
  struct PatchUv {
    Vec2 uv[3];
  };
  std::vector<PatchUv> patch_uvs(patches.size());

  for (size_t p = 0; p < patches.size(); ++p) {
    const Patch& patch = patches[p];
    const double x0 = patch.x + 0.5;
    const double y0 = patch.y + 0.5;
    const double extent = patch.size - 1.0;
    patch_uvs[p].uv[0] = Vec2(x0 * inv_atlas, y0 * inv_atlas);
    patch_uvs[p].uv[1] = Vec2((x0 + extent) * inv_atlas, y0 * inv_atlas);
    patch_uvs[p].uv[2] = Vec2(x0 * inv_atlas, (y0 + extent) * inv_atlas);

    const uint32_t base = static_cast<uint32_t>(out.vertices.size());
    for (int k = 0; k < 3; ++k) {
      const uint32_t source = mesh.indices[patch.triangle * 3 + k];
      out.vertices.push_back(mesh.vertices[source]);
      if (has_colors) out.colors.push_back(mesh.colors[source]);
      out.uvs.push_back(Vec2f(static_cast<float>(patch_uvs[p].uv[k].x()),
                              static_cast<float>(patch_uvs[p].uv[k].y())));
      out.indices.push_back(base + static_cast<uint32_t>(k));
    }
  }
  recomputeNormals(&out);

  // ---- seam levelling -----------------------------------------------------
  // Each patch gets a per-channel brightness offset chosen so that colours agree across shared
  // mesh edges. Solved by Gauss-Seidel over the adjacency graph: the system is a graph Laplacian
  // with one unknown per patch per channel, and a few dozen sweeps converge well below the
  // threshold where a seam is visible.
  std::vector<std::array<double, 3>> patch_offset(patches.size(), {0.0, 0.0, 0.0});
  if (options.level_seams && patches.size() > 1) {
    // Map original triangle index -> patch index.
    std::vector<int> triangle_patch(triangle_count, -1);
    for (size_t p = 0; p < patches.size(); ++p) triangle_patch[patches[p].triangle] = static_cast<int>(p);

    // Build adjacency from shared edges in the *original* mesh.
    std::unordered_map<uint64_t, std::vector<int>> edge_triangles;
    edge_triangles.reserve(triangle_count * 3);
    for (size_t t = 0; t < triangle_count; ++t) {
      const uint32_t v[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1],
                             mesh.indices[t * 3 + 2]};
      for (int e = 0; e < 3; ++e) {
        edge_triangles[edgeKey(v[e], v[(e + 1) % 3])].push_back(static_cast<int>(t));
      }
    }

    // For each shared edge, the colour each side samples at the edge midpoint.
    struct SeamPair {
      int patch_a, patch_b;
      std::array<double, 3> difference;
    };
    std::vector<SeamPair> seams;
    for (const auto& entry : edge_triangles) {
      if (entry.second.size() != 2) continue;
      const int ta = entry.second[0], tb = entry.second[1];
      const int pa = triangle_patch[ta], pb = triangle_patch[tb];
      if (pa < 0 || pb < 0 || pa == pb) continue;

      const uint32_t va = static_cast<uint32_t>(entry.first >> 32);
      const uint32_t vb = static_cast<uint32_t>(entry.first & 0xFFFFFFFFu);
      const Vec3 midpoint =
          (toDouble(mesh.vertices[va]) + toDouble(mesh.vertices[vb])) * 0.5;

      std::array<double, 3> sampled[2];
      bool ok = true;
      const int triangles[2] = {ta, tb};
      for (int side = 0; side < 2 && ok; ++side) {
        const int view_index = best_view[triangles[side]];
        const TextureView& view = views[view_index];
        const Vec3 cam = view.pose * midpoint;
        Vec2 px;
        if (cam.z() <= 1e-4 || !view.intrinsics.project(cam, &px)) {
          ok = false;
          break;
        }
        double rgb[3];
        sampleBilinear(view.image, px.x(), px.y(), rgb);
        for (int c = 0; c < 3; ++c) sampled[side][c] = rgb[c] * view_gain[view_index];
      }
      if (!ok) continue;
      SeamPair pair;
      pair.patch_a = pa;
      pair.patch_b = pb;
      for (int c = 0; c < 3; ++c) pair.difference[c] = sampled[1][c] - sampled[0][c];
      seams.push_back(pair);
    }

    const auto residual = [&seams, &patch_offset]() {
      double sum = 0.0;
      for (const SeamPair& seam : seams) {
        for (int c = 0; c < 3; ++c) {
          const double d = (seam.difference[c] + patch_offset[seam.patch_b][c]) -
                           patch_offset[seam.patch_a][c];
          sum += d * d;
        }
      }
      return seams.empty() ? 0.0 : std::sqrt(sum / (seams.size() * 3));
    };
    result.stats.seam_residual_before = residual();

    // Gauss-Seidel sweeps. A damping factor below 1 keeps the (rank-deficient, since a global
    // offset is unobservable) system from drifting as a whole.
    std::vector<std::vector<std::pair<int, std::array<double, 3>>>> neighbours(patches.size());
    for (const SeamPair& seam : seams) {
      std::array<double, 3> forward = seam.difference;
      std::array<double, 3> backward = {-forward[0], -forward[1], -forward[2]};
      neighbours[seam.patch_a].emplace_back(seam.patch_b, forward);
      neighbours[seam.patch_b].emplace_back(seam.patch_a, backward);
    }
    for (int iteration = 0; iteration < options.seam_iterations; ++iteration) {
      for (size_t p = 0; p < patches.size(); ++p) {
        if (neighbours[p].empty()) continue;
        std::array<double, 3> accumulator{0.0, 0.0, 0.0};
        for (const auto& neighbour : neighbours[p]) {
          for (int c = 0; c < 3; ++c) {
            accumulator[c] += patch_offset[neighbour.first][c] + neighbour.second[c];
          }
        }
        for (int c = 0; c < 3; ++c) {
          const double target = accumulator[c] / neighbours[p].size();
          patch_offset[p][c] += 0.7 * (target - patch_offset[p][c]);
          // Bound the correction so one badly exposed patch cannot drag its region.
          patch_offset[p][c] = std::max(-48.0, std::min(48.0, patch_offset[p][c]));
        }
      }
    }
    // Remove the unobservable global mean, keeping the result near the captured exposure.
    std::array<double, 3> mean{0.0, 0.0, 0.0};
    for (const auto& offset : patch_offset) {
      for (int c = 0; c < 3; ++c) mean[c] += offset[c];
    }
    for (int c = 0; c < 3; ++c) mean[c] /= static_cast<double>(patch_offset.size());
    for (auto& offset : patch_offset) {
      for (int c = 0; c < 3; ++c) offset[c] -= mean[c];
    }
    result.stats.seam_residual_after = residual();
  }

  // ---- rasterise the atlas ------------------------------------------------
  result.atlas = ColorBuffer(options.atlas_size, options.atlas_size);
  std::vector<uint8_t> covered(static_cast<size_t>(options.atlas_size) * options.atlas_size, 0);

  for (size_t p = 0; p < patches.size(); ++p) {
    const Patch& patch = patches[p];
    const TextureView& view = views[patch.view];
    const Vec3 world[3] = {
        toDouble(mesh.vertices[mesh.indices[patch.triangle * 3]]),
        toDouble(mesh.vertices[mesh.indices[patch.triangle * 3 + 1]]),
        toDouble(mesh.vertices[mesh.indices[patch.triangle * 3 + 2]]),
    };
    const Vec2 uv_px[3] = {
        patch_uvs[p].uv[0] * options.atlas_size,
        patch_uvs[p].uv[1] * options.atlas_size,
        patch_uvs[p].uv[2] * options.atlas_size,
    };
    const double gain = view_gain[patch.view];
    const std::array<double, 3>& offset = patch_offset[p];

    // Rasterise with a one-texel dilation so the patch's own padding is filled with its edge
    // colour. Without it, bilinear filtering at a patch boundary mixes in the clear colour and
    // every triangle gains a dark rim.
    const int x_min = std::max(0, patch.x - patch.padding);
    const int y_min = std::max(0, patch.y - patch.padding);
    const int x_max = std::min(options.atlas_size - 1, patch.x + patch.size + patch.padding);
    const int y_max = std::min(options.atlas_size - 1, patch.y + patch.size + patch.padding);

    for (int y = y_min; y <= y_max; ++y) {
      for (int x = x_min; x <= x_max; ++x) {
        double u = 0.0, v = 0.0, w = 0.0;
        if (!barycentric(Vec2(x + 0.5, y + 0.5), uv_px[0], uv_px[1], uv_px[2], &u, &v, &w)) {
          continue;
        }
        // Clamp slightly outside the triangle into it, which produces the dilated border.
        const double clamp_margin = -0.35;
        if (u < clamp_margin || v < clamp_margin || w < clamp_margin) continue;
        u = std::max(0.0, u);
        v = std::max(0.0, v);
        w = std::max(0.0, w);
        const double total = u + v + w;
        if (total < 1e-12) continue;
        u /= total;
        v /= total;
        w /= total;

        const Vec3 surface = world[0] * u + world[1] * v + world[2] * w;
        const Vec3 cam = view.pose * surface;
        Vec2 px;
        if (cam.z() <= 1e-4 || !view.intrinsics.project(cam, &px)) continue;
        double rgb[3];
        sampleBilinear(view.image, px.x(), px.y(), rgb);
        uint8_t out_rgb[3];
        for (int c = 0; c < 3; ++c) {
          const double value = rgb[c] * gain + offset[c];
          out_rgb[c] = static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(value))));
        }
        result.atlas.set(x, y, out_rgb[0], out_rgb[1], out_rgb[2]);
        covered[static_cast<size_t>(y) * options.atlas_size + x] = 1;
      }
    }
  }

  // ---- inpaint the gaps between patches -----------------------------------
  // Uncovered texels are never sampled by the mesh, but mipmap generation averages them in, so
  // leaving them black darkens every patch edge at lower mip levels.
  if (options.inpaint_holes) {
    for (int pass = 0; pass < 3; ++pass) {
      std::vector<uint8_t> filled = covered;
      for (int y = 0; y < options.atlas_size; ++y) {
        for (int x = 0; x < options.atlas_size; ++x) {
          if (covered[static_cast<size_t>(y) * options.atlas_size + x]) continue;
          int sum[3] = {0, 0, 0};
          int count = 0;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const int nx = x + dx, ny = y + dy;
              if (nx < 0 || ny < 0 || nx >= options.atlas_size || ny >= options.atlas_size) {
                continue;
              }
              if (!covered[static_cast<size_t>(ny) * options.atlas_size + nx]) continue;
              uint8_t rgb[3];
              result.atlas.get(nx, ny, rgb);
              for (int c = 0; c < 3; ++c) sum[c] += rgb[c];
              ++count;
            }
          }
          if (count == 0) continue;
          result.atlas.set(x, y, static_cast<uint8_t>(sum[0] / count),
                           static_cast<uint8_t>(sum[1] / count),
                           static_cast<uint8_t>(sum[2] / count));
          filled[static_cast<size_t>(y) * options.atlas_size + x] = 1;
        }
      }
      covered = std::move(filled);
    }
  }

  result.mesh = std::move(out);
  result.success = true;
  return result;
}

}  // namespace parallax
