#include "parallax/depth/MultiViewStereo.h"

#include "parallax/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace parallax {
namespace {

/// Below this, a patch has no texture to match and NCC against it is noise.
constexpr double kMinPatchNorm = 12.0;

/// The baseline-to-depth ratio that triangulates best in practice.
constexpr double kIdealBaselineRatio = 0.1;

/// Views whose optical axes differ by more than about 60 degrees see different
/// surfaces, so matching them is a waste of samples.
constexpr double kMinViewAlignment = 0.5;

constexpr size_t kMinPointsForRange = 12;
constexpr double kMinRangeMargin = 0.1;
constexpr double kAbsoluteMinDepth = 0.1;

/// A median needs a real neighbourhood; below this the pixel stays unmeasured.
constexpr size_t kMinMedianSupport = 5;

/// Bilinear sample of a greyscale image. Returns false outside the image.
bool sampleBilinear(const GrayBuffer& image, double x, double y, double* out) {
  if (x < 0.0 || y < 0.0 || x > image.width - 1.0 || y > image.height - 1.0) return false;
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, image.width - 1);
  const int y1 = std::min(y0 + 1, image.height - 1);
  const double fx = x - x0;
  const double fy = y - y0;
  const double top = image.at(x0, y0) * (1.0 - fx) + image.at(x1, y0) * fx;
  const double bottom = image.at(x0, y1) * (1.0 - fx) + image.at(x1, y1) * fx;
  *out = top * (1.0 - fy) + bottom * fy;
  return true;
}

/// Downscale by an integer factor with box averaging.
GrayBuffer downscale(const GrayBuffer& source, int factor) {
  if (factor <= 1) return source;
  const int width = std::max(1, source.width / factor);
  const int height = std::max(1, source.height / factor);
  GrayBuffer out(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int sum = 0;
      int count = 0;
      for (int dy = 0; dy < factor; ++dy) {
        for (int dx = 0; dx < factor; ++dx) {
          const int sx = x * factor + dx;
          const int sy = y * factor + dy;
          if (sx < source.width && sy < source.height) {
            sum += source.at(sx, sy);
            ++count;
          }
        }
      }
      out.at(x, y) = static_cast<uint8_t>(count > 0 ? sum / count : 0);
    }
  }
  return out;
}

/// A patch of the reference image, pre-centred so NCC is one dot product.
struct ReferencePatch {
  std::vector<double> centred;
  double norm = 0.0;
  bool valid = false;
};

ReferencePatch buildPatch(const GrayBuffer& image, int cx, int cy, int radius) {
  ReferencePatch patch;
  const int side = 2 * radius + 1;
  patch.centred.resize(static_cast<size_t>(side) * side);
  double sum = 0.0;
  size_t index = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = std::clamp(cx + dx, 0, image.width - 1);
      const int y = std::clamp(cy + dy, 0, image.height - 1);
      const double value = image.at(x, y);
      patch.centred[index++] = value;
      sum += value;
    }
  }
  const double mean = sum / patch.centred.size();
  double sum_squares = 0.0;
  for (double& value : patch.centred) {
    value -= mean;
    sum_squares += value * value;
  }
  patch.norm = std::sqrt(sum_squares);
  // A flat patch has no texture to match. Correlating it against anything gives
  // a meaningless answer with a confident-looking score, so it is rejected
  // here rather than filtered out later.
  patch.valid = patch.norm > kMinPatchNorm;
  return patch;
}

/**
 * Normalised cross-correlation of the reference patch against a neighbour,
 * sampled at the pixels a given depth hypothesis maps to.
 *
 * Returns a score in [-1, 1], or -2 when the patch falls outside the neighbour.
 */
double correlate(const ReferencePatch& patch, const GrayBuffer& neighbour,
                 const CameraIntrinsics& reference_intrinsics, const Pose& neighbour_from_reference,
                 const CameraIntrinsics& neighbour_intrinsics, int cx, int cy, int radius,
                 double depth) {
  const int side = 2 * radius + 1;
  std::vector<double> samples(static_cast<size_t>(side) * side);
  double sum = 0.0;
  size_t index = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      // Each pixel of the patch is back-projected at the same depth, which
      // assumes the patch lies on a fronto-parallel plane. That is the standard
      // plane-sweep assumption; it costs accuracy on strongly slanted surfaces
      // and buys a cost volume that can be evaluated without estimating a
      // normal per pixel.
      const Vec3 ray = reference_intrinsics.unproject(Vec2(cx + dx, cy + dy));
      const Vec3 point_reference = ray * depth;
      const Vec3 point_neighbour = neighbour_from_reference * point_reference;
      Vec2 pixel;
      if (!neighbour_intrinsics.project(point_neighbour, &pixel)) return -2.0;
      double value = 0.0;
      if (!sampleBilinear(neighbour, pixel.x(), pixel.y(), &value)) return -2.0;
      samples[index++] = value;
      sum += value;
    }
  }
  const double mean = sum / samples.size();
  double dot = 0.0;
  double sum_squares = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double centred = samples[i] - mean;
    dot += patch.centred[i] * centred;
    sum_squares += centred * centred;
  }
  const double norm = std::sqrt(sum_squares);
  if (norm < kMinPatchNorm) return -2.0;
  return dot / (patch.norm * norm);
}

/// Median of a small set, used to smooth the winner-takes-all surface.
float medianOf(std::vector<float>& values) {
  if (values.empty()) return 0.f;
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  return values[middle];
}

}  // namespace

std::vector<int> selectNeighbours(const std::vector<MvsView>& views, int reference_index,
                                  int count, double scene_depth) {
  std::vector<int> chosen;
  if (reference_index < 0 || reference_index >= static_cast<int>(views.size())) return chosen;
  if (scene_depth <= 0.0) scene_depth = 1.0;

  const MvsView& reference = views[reference_index];
  const Vec3 reference_centre = reference.pose.center();
  const Vec3 reference_forward = reference.pose.forward();

  struct Candidate {
    int index;
    double score;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(views.size());

  for (size_t i = 0; i < views.size(); ++i) {
    if (static_cast<int>(i) == reference_index) continue;
    if (views[i].gray == nullptr) continue;
    const Vec3 centre = views[i].pose.center();
    const double baseline = (centre - reference_centre).norm();
    if (baseline < 1e-6) continue;

    // The baseline-to-depth ratio is what actually determines triangulation
    // accuracy. Too small and the depth is unconstrained; too large and the
    // patches no longer look alike. The sweet spot is around a tenth.
    const double ratio = baseline / scene_depth;
    const double ratio_score =
        std::exp(-std::pow(std::log(ratio / kIdealBaselineRatio), 2.0) / (2.0 * 0.5 * 0.5));

    // Views looking from a very different direction see different surfaces.
    const double alignment = reference_forward.dot(views[i].pose.forward());
    if (alignment < kMinViewAlignment) continue;

    candidates.push_back({static_cast<int>(i), ratio_score * alignment});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  for (int i = 0; i < static_cast<int>(candidates.size()) && i < count; ++i) {
    chosen.push_back(candidates[i].index);
  }
  return chosen;
}

bool depthRangeFromPoints(const std::vector<Vec3>& world_points, const Pose& pose,
                          const CameraIntrinsics& intrinsics, double* out_min, double* out_max) {
  std::vector<double> depths;
  depths.reserve(world_points.size());
  for (const Vec3& point : world_points) {
    const Vec3 in_camera = pose * point;
    if (in_camera.z() <= 0.0) continue;
    Vec2 pixel;
    if (!intrinsics.project(in_camera, &pixel)) continue;
    if (pixel.x() < 0 || pixel.y() < 0 || pixel.x() >= intrinsics.width ||
        pixel.y() >= intrinsics.height) {
      continue;
    }
    depths.push_back(in_camera.z());
  }
  if (depths.size() < kMinPointsForRange) return false;

  std::sort(depths.begin(), depths.end());
  // Percentiles rather than the extremes: one mis-triangulated point at 400 m
  // would otherwise stretch the sweep so far that every sample is wasted.
  const double low = depths[static_cast<size_t>(depths.size() * 0.05)];
  const double high = depths[static_cast<size_t>(depths.size() * 0.95)];
  const double margin = std::max(kMinRangeMargin, (high - low) * 0.25);
  *out_min = std::max(kAbsoluteMinDepth, low - margin);
  *out_max = std::max(*out_min + kMinRangeMargin, high + margin);
  return true;
}

DepthMap estimateDepth(const MvsView& reference, const std::vector<MvsView>& neighbours,
                       const MvsConfig& config) {
  DepthMap result;
  if (reference.gray == nullptr || !reference.intrinsics.valid()) return result;
  if (neighbours.empty()) return result;
  if (config.depth_samples < 2 || config.max_depth <= config.min_depth) return result;

  const int factor = std::max(1, config.downscale);
  const GrayBuffer reference_image = downscale(*reference.gray, factor);
  const CameraIntrinsics reference_intrinsics =
      reference.intrinsics.scaledTo(reference_image.width, reference_image.height);

  // Each neighbour is pre-transformed into the reference's frame, and its
  // intrinsics rescaled to its own downscaled image. Doing this once per
  // neighbour rather than once per pixel is the difference between a second
  // and a minute per frame.
  struct PreparedNeighbour {
    GrayBuffer image;
    CameraIntrinsics intrinsics;
    Pose neighbour_from_reference;
    double baseline = 0.0;
  };
  std::vector<PreparedNeighbour> prepared;
  prepared.reserve(neighbours.size());
  for (const MvsView& neighbour : neighbours) {
    if (neighbour.gray == nullptr || !neighbour.intrinsics.valid()) continue;
    PreparedNeighbour entry;
    entry.image = downscale(*neighbour.gray, factor);
    entry.intrinsics = neighbour.intrinsics.scaledTo(entry.image.width, entry.image.height);
    // reference is camera-from-world, so reference.inverse() is world-from-reference.
    entry.neighbour_from_reference = neighbour.pose * reference.pose.inverse();
    entry.baseline = (neighbour.pose.center() - reference.pose.center()).norm();
    if (entry.baseline < 1e-6) continue;
    prepared.push_back(std::move(entry));
  }
  if (prepared.empty()) return result;

  // Hypotheses are spaced uniformly in *inverse* depth. Depth error from a
  // fixed pixel disparity grows with the square of the depth, so uniform depth
  // spacing wastes most of its samples on the far field where they cannot be
  // distinguished anyway.
  std::vector<double> hypotheses(config.depth_samples);
  const double inverse_near = 1.0 / config.max_depth;
  const double inverse_far = 1.0 / config.min_depth;
  for (int i = 0; i < config.depth_samples; ++i) {
    const double t = static_cast<double>(i) / (config.depth_samples - 1);
    hypotheses[i] = 1.0 / (inverse_near + t * (inverse_far - inverse_near));
  }

  const int radius = std::max(1, config.window_radius);
  DepthMap raw(reference_image.width, reference_image.height);
  raw.confidence.assign(
      static_cast<size_t>(reference_image.width) * reference_image.height, 0);

  // Row blocks across the cores. Each pixel writes only its own entry of `raw`, reads only
  // the images, and depends on no other pixel's answer.
  parallelFor(static_cast<size_t>(reference_image.height), config.threads,
              [&](size_t row_begin, size_t row_end) {
  for (int y = static_cast<int>(row_begin); y < static_cast<int>(row_end); ++y) {
    for (int x = 0; x < reference_image.width; ++x) {
      const ReferencePatch patch = buildPatch(reference_image, x, y, radius);
      if (!patch.valid) continue;

      double best_score = config.min_correlation;
      double best_depth = 0.0;
      for (double depth : hypotheses) {
        // The score for a hypothesis is the *sum* over neighbours that could
        // see it, not the best single one: a single agreeing view is exactly
        // what a repeated texture produces, and summing makes the agreement of
        // several views the thing that wins.
        double total = 0.0;
        int contributing = 0;
        for (const PreparedNeighbour& neighbour : prepared) {
          const double score =
              correlate(patch, neighbour.image, reference_intrinsics,
                        neighbour.neighbour_from_reference, neighbour.intrinsics, x, y, radius,
                        depth);
          if (score < -1.5) continue;
          total += score;
          ++contributing;
        }
        if (contributing == 0) continue;
        const double mean = total / contributing;
        if (mean > best_score) {
          best_score = mean;
          best_depth = depth;
        }
      }
      if (best_depth > 0.0) {
        raw.at(x, y) = static_cast<float>(best_depth);
        raw.confidence[static_cast<size_t>(y) * raw.width + x] =
            static_cast<uint8_t>(std::clamp(best_score, 0.0, 1.0) * 255.0);
      }
    }
  }
  });

  // ---- geometric consistency -------------------------------------------
  //
  // A depth is kept only if reprojecting it into the neighbours and comparing
  // against *their* estimates agrees. Without this the output is riddled with
  // confident wrong depths from repeated texture, and a confident wrong depth
  // is worse than a missing one: TSDF integration carves a hole through the
  // real surface for it.
  //
  // The neighbours' own depth estimates are not available here -- estimating
  // them would be recursive -- so the check is the weaker but still effective
  // one: the depth must be consistent with the *photometric* evidence from
  // each neighbour individually, counted per view.
  DepthMap filtered(raw.width, raw.height);
  filtered.confidence.assign(raw.confidence.size(), 0);
  parallelFor(static_cast<size_t>(raw.height), config.threads,
              [&](size_t row_begin, size_t row_end) {
  for (int y = static_cast<int>(row_begin); y < static_cast<int>(row_end); ++y) {
    for (int x = 0; x < raw.width; ++x) {
      const float depth = raw.at(x, y);
      if (depth <= 0.f) continue;
      const ReferencePatch patch = buildPatch(reference_image, x, y, radius);
      if (!patch.valid) continue;

      int agreeing = 0;
      for (const PreparedNeighbour& neighbour : prepared) {
        const double score =
            correlate(patch, neighbour.image, reference_intrinsics,
                      neighbour.neighbour_from_reference, neighbour.intrinsics, x, y, radius,
                      depth);
        if (score >= config.min_correlation) ++agreeing;
      }
      if (agreeing >= config.min_consistent_views) {
        filtered.at(x, y) = depth;
        filtered.confidence[static_cast<size_t>(y) * filtered.width + x] =
            raw.confidence[static_cast<size_t>(y) * raw.width + x];
      }
    }
  }
  });

  // ---- median smoothing -------------------------------------------------
  //
  // Winner-takes-all over a discrete set of hypotheses is inherently blocky.
  // A small median filter removes the single-pixel outliers without the edge
  // blurring a mean filter would cause, which matters because the edges are
  // where the surface actually is.
  result = DepthMap(filtered.width, filtered.height);
  result.confidence.assign(filtered.confidence.size(), 0);
  std::vector<float> window;
  window.reserve(9);
  for (int y = 0; y < filtered.height; ++y) {
    for (int x = 0; x < filtered.width; ++x) {
      window.clear();
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int sx = x + dx;
          const int sy = y + dy;
          if (sx < 0 || sy < 0 || sx >= filtered.width || sy >= filtered.height) continue;
          const float value = filtered.at(sx, sy);
          if (value > 0.f) window.push_back(value);
        }
      }
      // A pixel whose neighbourhood is mostly unmeasured is left unmeasured:
      // filling it in from one or two samples invents surface.
      if (window.size() < kMinMedianSupport) continue;
      result.at(x, y) = medianOf(window);
      result.confidence[static_cast<size_t>(y) * result.width + x] =
          filtered.confidence[static_cast<size_t>(y) * filtered.width + x];
    }
  }
  return result;
}

}  // namespace parallax
