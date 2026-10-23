#include "parallax/features/Features.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace parallax {
namespace {

// Bresenham circle of radius 3 (the 16 pixels the FAST-9 test inspects).
constexpr int kCircleX[16] = {0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -2, -1};
constexpr int kCircleY[16] = {-3, -3, -2, -1, 0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3};

/// The 256 BRIEF sampling pairs. Generated once, deterministically, by drawing
/// from an isotropic Gaussian (sigma = patch/5) and rejecting points outside the
/// 31x31 patch -- the recipe from the original BRIEF/ORB papers. Keeping the
/// table generated rather than hard-coded means the descriptor is reproducible
/// across platforms without shipping 1 KB of magic numbers.
struct BriefPattern {
  std::array<int8_t, 256 * 4> offsets{};

  BriefPattern() {
    // xorshift64 keeps the sequence identical on every platform, unlike
    // std::mt19937 + normal_distribution whose output is implementation defined.
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    };
    // Box-Muller from two uniforms, clamped to the patch half-width of 15.
    auto gauss = [&next]() {
      const double u1 = ((next() >> 11) + 1) / 9007199254740994.0;  // (0,1)
      const double u2 = ((next() >> 11) + 1) / 9007199254740994.0;
      const double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
      const double scaled = g * (31.0 / 5.0);
      return static_cast<int8_t>(std::max(-15.0, std::min(15.0, scaled)));
    };
    for (int i = 0; i < 256; ++i) {
      offsets[i * 4 + 0] = gauss();
      offsets[i * 4 + 1] = gauss();
      offsets[i * 4 + 2] = gauss();
      offsets[i * 4 + 3] = gauss();
    }
  }
};

const BriefPattern& briefPattern() {
  static const BriefPattern pattern;
  return pattern;
}

}  // namespace

// ------------------------------------------------------------------ filtering

GrayBuffer gaussianBlur5(const GrayImage& src) {
  // Normalised 5-tap binomial kernel [1 4 6 4 1] / 16, applied separably.
  static constexpr int kK[5] = {1, 4, 6, 4, 1};
  GrayBuffer tmp(src.width, src.height);
  GrayBuffer out(src.width, src.height);

  for (int y = 0; y < src.height; ++y) {
    for (int x = 0; x < src.width; ++x) {
      int acc = 0;
      for (int i = -2; i <= 2; ++i) {
        const int xx = std::min(src.width - 1, std::max(0, x + i));
        acc += kK[i + 2] * src.at(xx, y);
      }
      tmp.at(x, y) = static_cast<uint8_t>((acc + 8) >> 4);
    }
  }
  for (int y = 0; y < src.height; ++y) {
    for (int x = 0; x < src.width; ++x) {
      int acc = 0;
      for (int i = -2; i <= 2; ++i) {
        const int yy = std::min(src.height - 1, std::max(0, y + i));
        acc += kK[i + 2] * tmp.at(x, yy);
      }
      out.at(x, y) = static_cast<uint8_t>((acc + 8) >> 4);
    }
  }
  return out;
}

GrayBuffer downsampleHalf(const GrayImage& src) {
  const GrayBuffer blurred = gaussianBlur5(src);
  const int w = std::max(1, src.width / 2);
  const int h = std::max(1, src.height / 2);
  GrayBuffer out(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int x0 = std::min(src.width - 1, x * 2);
      const int y0 = std::min(src.height - 1, y * 2);
      const int x1 = std::min(src.width - 1, x0 + 1);
      const int y1 = std::min(src.height - 1, y0 + 1);
      const int acc = blurred.at(x0, y0) + blurred.at(x1, y0) +
                      blurred.at(x0, y1) + blurred.at(x1, y1);
      out.at(x, y) = static_cast<uint8_t>((acc + 2) >> 2);
    }
  }
  return out;
}

GrayBuffer resizeBilinear(const GrayImage& src, int out_w, int out_h) {
  GrayBuffer out(std::max(1, out_w), std::max(1, out_h));
  const double sx = static_cast<double>(src.width) / out.width;
  const double sy = static_cast<double>(src.height) / out.height;
  for (int y = 0; y < out.height; ++y) {
    const double fy = std::min(src.height - 1.001, (y + 0.5) * sy - 0.5);
    for (int x = 0; x < out.width; ++x) {
      const double fx = std::min(src.width - 1.001, (x + 0.5) * sx - 0.5);
      const double v = src.sample(std::max(0.0, fx), std::max(0.0, fy));
      out.at(x, y) = static_cast<uint8_t>(std::lround(std::max(0.0, std::min(255.0, v))));
    }
  }
  return out;
}

ImagePyramid::ImagePyramid(const GrayImage& base, int octaves, double scale_factor) {
  octaves = std::max(1, octaves);
  levels_.reserve(octaves);
  scales_.reserve(octaves);
  levels_.emplace_back(base.width, base.height);
  for (int y = 0; y < base.height; ++y) {
    for (int x = 0; x < base.width; ++x) levels_[0].at(x, y) = base.at(x, y);
  }
  scales_.push_back(1.0);

  for (int i = 1; i < octaves; ++i) {
    const double scale = std::pow(scale_factor, i);
    const int w = static_cast<int>(std::lround(base.width / scale));
    const int h = static_cast<int>(std::lround(base.height / scale));
    if (w < 32 || h < 32) break;
    // Resampling from the previous level keeps the effective blur consistent
    // with the sampling rate, which matters for descriptor repeatability.
    levels_.push_back(resizeBilinear(gaussianBlur5(levels_.back().view()).view(), w, h));
    scales_.push_back(static_cast<double>(base.width) / w);
  }
}

// ------------------------------------------------------------------ detection

int fastScore(const GrayImage& img, int x, int y, int threshold) {
  if (!img.inside(x, y, 3)) return 0;
  const int c = img.at(x, y);
  const int hi = c + threshold;
  const int lo = c - threshold;

  // Fast rejection using the 4 compass points (indices 0, 4, 8, 12).
  //
  // The arc we are looking for spans 9 of the 16 ring positions, and the
  // compass points are spaced 4 apart, so any valid 9-long window must contain
  // at least floor(9 / 4) = 2 of them. Two is therefore the strongest
  // pre-condition that cannot reject a true corner. (The widely quoted
  // "3 of 4" test belongs to FAST-12, whose 12-long arc does guarantee three
  // compass points; applying it to FAST-9 silently discards genuine corners,
  // notably the corners of axis-aligned rectangles.)
  int bright_compass = 0, dark_compass = 0;
  for (int i = 0; i < 16; i += 4) {
    const int v = img.at(x + kCircleX[i], y + kCircleY[i]);
    if (v > hi) ++bright_compass;
    else if (v < lo) ++dark_compass;
  }
  if (bright_compass < 2 && dark_compass < 2) return 0;

  int ring[16];
  for (int i = 0; i < 16; ++i) ring[i] = img.at(x + kCircleX[i], y + kCircleY[i]);

  // Longest contiguous run (circular) of brighter-than-hi or darker-than-lo.
  auto longestRun = [&ring](bool bright, int hi_v, int lo_v) {
    int best = 0, run = 0;
    // Walk twice around to let runs wrap past index 15.
    for (int i = 0; i < 32; ++i) {
      const int v = ring[i & 15];
      const bool hit = bright ? (v > hi_v) : (v < lo_v);
      run = hit ? run + 1 : 0;
      best = std::max(best, std::min(run, 16));
    }
    return best;
  };
  return std::max(longestRun(true, hi, lo), longestRun(false, hi, lo));
}

double harrisResponse(const GrayImage& img, int x, int y) {
  if (!img.inside(x, y, 4)) return 0.0;
  // Structure tensor over a 7x7 window of Sobel gradients.
  double a = 0.0, b = 0.0, c = 0.0;
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      const int px = x + dx, py = y + dy;
      const double gx =
          (img.at(px + 1, py - 1) + 2.0 * img.at(px + 1, py) + img.at(px + 1, py + 1)) -
          (img.at(px - 1, py - 1) + 2.0 * img.at(px - 1, py) + img.at(px - 1, py + 1));
      const double gy =
          (img.at(px - 1, py + 1) + 2.0 * img.at(px, py + 1) + img.at(px + 1, py + 1)) -
          (img.at(px - 1, py - 1) + 2.0 * img.at(px, py - 1) + img.at(px + 1, py - 1));
      a += gx * gx;
      b += gy * gy;
      c += gx * gy;
    }
  }
  // Harris: det(M) - k * trace(M)^2 with the usual k = 0.04. Scaled down to
  // keep the magnitudes in a range that prints readably.
  const double det = a * b - c * c;
  const double trace = a + b;
  return (det - 0.04 * trace * trace) * 1e-9;
}

double intensityCentroidAngle(const GrayImage& img, int x, int y, int radius) {
  // ORB's orientation estimate: the vector from the patch centre to its
  // intensity centroid. Rotation-covariant and extremely cheap.
  double m01 = 0.0, m10 = 0.0;
  const int r2 = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy) {
    const int py = y + dy;
    if (py < 0 || py >= img.height) continue;
    const int span = static_cast<int>(std::sqrt(static_cast<double>(r2 - dy * dy)));
    for (int dx = -span; dx <= span; ++dx) {
      const int px = x + dx;
      if (px < 0 || px >= img.width) continue;
      const double v = img.at(px, py);
      m10 += dx * v;
      m01 += dy * v;
    }
  }
  if (std::abs(m10) < 1e-9 && std::abs(m01) < 1e-9) return 0.0;
  return std::atan2(m01, m10);
}

std::vector<Keypoint> detectFast(const GrayImage& img, const FeatureParams& params) {
  std::vector<Keypoint> raw;
  if (!img.valid()) return raw;

  const int border = std::max(4, params.border);
  if (img.width <= 2 * border || img.height <= 2 * border) return raw;

  // Two passes: the nominal threshold, then a relaxed one if a region produced
  // nothing. This is what keeps low-texture walls from going completely blind.
  auto collect = [&](int threshold, std::vector<Keypoint>* out) {
    for (int y = border; y < img.height - border; ++y) {
      for (int x = border; x < img.width - border; ++x) {
        if (fastScore(img, x, y, threshold) < 9) continue;
        Keypoint kp;
        kp.pt = Vec2(x, y);
        kp.score = static_cast<float>(harrisResponse(img, x, y));
        out->push_back(kp);
      }
    }
  };
  collect(params.fast_threshold, &raw);
  if (raw.size() < static_cast<size_t>(params.max_features / 4) &&
      params.min_fast_threshold < params.fast_threshold) {
    raw.clear();
    collect(params.min_fast_threshold, &raw);
  }
  if (raw.empty()) return raw;

  // Non-maximum suppression on a 3x3 neighbourhood of the Harris score.
  // Implemented via a score grid rather than pairwise comparison so the cost
  // stays linear in the number of candidates.
  std::vector<float> score_map(static_cast<size_t>(img.width) * img.height, 0.f);
  for (const Keypoint& kp : raw) {
    score_map[static_cast<size_t>(kp.pt.y()) * img.width + static_cast<int>(kp.pt.x())] =
        kp.score;
  }
  std::vector<Keypoint> suppressed;
  suppressed.reserve(raw.size());
  for (const Keypoint& kp : raw) {
    const int x = static_cast<int>(kp.pt.x());
    const int y = static_cast<int>(kp.pt.y());
    bool is_max = true;
    for (int dy = -1; dy <= 1 && is_max; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const float neighbour =
            score_map[static_cast<size_t>(y + dy) * img.width + (x + dx)];
        // Strict > on one side and >= on the other breaks ties deterministically
        // so exactly one pixel of a plateau survives.
        if (neighbour > kp.score || (neighbour == kp.score && (dy < 0 || (dy == 0 && dx < 0)))) {
          is_max = false;
          break;
        }
      }
    }
    if (is_max) suppressed.push_back(kp);
  }
  return suppressed;
}

// ----------------------------------------------------------------- describing

Descriptor computeBrief(const GrayImage& img, double x, double y, double angle) {
  Descriptor d;
  const double ca = std::cos(angle);
  const double sa = std::sin(angle);
  const auto& pattern = briefPattern().offsets;

  for (int i = 0; i < 256; ++i) {
    const double ax = pattern[i * 4 + 0];
    const double ay = pattern[i * 4 + 1];
    const double bx = pattern[i * 4 + 2];
    const double by = pattern[i * 4 + 3];
    // Steer the sampling pair by the keypoint orientation.
    const double pax = x + ca * ax - sa * ay;
    const double pay = y + sa * ax + ca * ay;
    const double pbx = x + ca * bx - sa * by;
    const double pby = y + sa * bx + ca * by;

    const double va = img.sample(std::max(0.0, std::min<double>(img.width - 2, pax)),
                                 std::max(0.0, std::min<double>(img.height - 2, pay)));
    const double vb = img.sample(std::max(0.0, std::min<double>(img.width - 2, pbx)),
                                 std::max(0.0, std::min<double>(img.height - 2, pby)));
    if (va < vb) d.bits[i >> 6] |= (1ull << (i & 63));
  }
  return d;
}

FeatureSet detectAndDescribe(const GrayImage& img, const FeatureParams& params) {
  FeatureSet out;
  if (!img.valid()) return out;

  const ImagePyramid pyramid(img, params.octaves, params.scale_factor);

  // Distribute the feature budget across levels, weighted by area so finer
  // levels (which carry more detail) get proportionally more corners.
  std::vector<Keypoint> all;
  for (int level = 0; level < pyramid.levels(); ++level) {
    const GrayBuffer& lvl = pyramid.level(level);
    FeatureParams lp = params;
    lp.border = std::max(20, params.border);
    std::vector<Keypoint> kps = detectFast(lvl.view(), lp);
    const double scale = pyramid.scaleToBase(level);
    for (Keypoint& kp : kps) {
      kp.octave = level;
      kp.angle = static_cast<float>(
          intensityCentroidAngle(lvl.view(), static_cast<int>(kp.pt.x()),
                                 static_cast<int>(kp.pt.y())));
      // Record the level-local position in `pt` for descriptor extraction and
      // rescale after the budget selection below.
      all.push_back(kp);
      all.back().score = kp.score / static_cast<float>(scale);  // prefer coarse-stable corners
    }
  }
  if (all.empty()) return out;

  // Spatial distribution. Corners are binned into a grid and the budget is
  // drained round-robin: every cell contributes its strongest corner, then its
  // second strongest, and so on. A cell that runs dry simply stops
  // contributing, so the budget is always filled, but a single high-texture
  // region can never crowd out the rest of the frame. Taking the globally
  // strongest corners as a top-up instead would defeat the binning entirely,
  // and poorly spread correspondences are what makes bundle adjustment
  // ill-conditioned.
  const int cols = std::max(1, params.grid_cols);
  const int rows = std::max(1, params.grid_rows);
  std::vector<std::vector<const Keypoint*>> cells(static_cast<size_t>(cols) * rows);
  for (const Keypoint& kp : all) {
    const double scale = pyramid.scaleToBase(kp.octave);
    const double bx = kp.pt.x() * scale;
    const double by = kp.pt.y() * scale;
    const int cx = std::min(cols - 1, std::max(0, static_cast<int>(bx * cols / img.width)));
    const int cy = std::min(rows - 1, std::max(0, static_cast<int>(by * rows / img.height)));
    cells[static_cast<size_t>(cy) * cols + cx].push_back(&kp);
  }
  for (auto& cell : cells) {
    std::sort(cell.begin(), cell.end(),
              [](const Keypoint* a, const Keypoint* b) { return a->score > b->score; });
  }

  const size_t budget = static_cast<size_t>(std::max(1, params.max_features));
  std::vector<const Keypoint*> selected;
  selected.reserve(budget);
  for (size_t round = 0; selected.size() < budget; ++round) {
    bool progressed = false;
    for (auto& cell : cells) {
      if (round >= cell.size()) continue;
      selected.push_back(cell[round]);
      progressed = true;
      if (selected.size() >= budget) break;
    }
    if (!progressed) break;  // every cell exhausted
  }
  // Report strongest first; downstream stages truncate from the front.
  std::sort(selected.begin(), selected.end(),
            [](const Keypoint* a, const Keypoint* b) { return a->score > b->score; });

  out.keypoints.reserve(selected.size());
  out.descriptors.reserve(selected.size());
  for (const Keypoint* kp : selected) {
    const GrayBuffer& lvl = pyramid.level(kp->octave);
    out.descriptors.push_back(computeBrief(lvl.view(), kp->pt.x(), kp->pt.y(), kp->angle));
    Keypoint scaled = *kp;
    const double scale = pyramid.scaleToBase(kp->octave);
    scaled.pt = kp->pt * scale;  // report in level-0 pixels
    out.keypoints.push_back(scaled);
  }
  return out;
}

// ------------------------------------------------------------------- matching

std::vector<Match> matchFeatures(const FeatureSet& a, const FeatureSet& b,
                                 const MatchParams& params) {
  std::vector<Match> matches;
  if (a.empty() || b.empty()) return matches;

  auto bestTwo = [](const Descriptor& d, const std::vector<Descriptor>& set, int* best_idx,
                    int* best_dist, int* second_dist) {
    *best_idx = -1;
    *best_dist = 257;
    *second_dist = 257;
    for (size_t i = 0; i < set.size(); ++i) {
      const int dist = d.distance(set[i]);
      if (dist < *best_dist) {
        *second_dist = *best_dist;
        *best_dist = dist;
        *best_idx = static_cast<int>(i);
      } else if (dist < *second_dist) {
        *second_dist = dist;
      }
    }
  };

  // Forward pass with ratio test.
  std::vector<int> forward(a.size(), -1);
  std::vector<int> forward_dist(a.size(), 257);
  for (size_t i = 0; i < a.size(); ++i) {
    int idx, d1, d2;
    bestTwo(a.descriptors[i], b.descriptors, &idx, &d1, &d2);
    if (idx < 0 || d1 > params.max_distance) continue;
    // Lowe's ratio test. d2 == 257 means b had a single candidate, which we
    // accept on distance alone.
    if (d2 < 257 && d1 > params.ratio * d2) continue;
    forward[i] = idx;
    forward_dist[i] = d1;
  }
  if (!params.cross_check) {
    for (size_t i = 0; i < a.size(); ++i) {
      if (forward[i] >= 0) matches.push_back({static_cast<int>(i), forward[i], forward_dist[i]});
    }
    return matches;
  }

  // Backward pass; keep only mutual nearest neighbours.
  std::vector<int> backward(b.size(), -1);
  for (size_t j = 0; j < b.size(); ++j) {
    int idx, d1, d2;
    bestTwo(b.descriptors[j], a.descriptors, &idx, &d1, &d2);
    if (idx < 0 || d1 > params.max_distance) continue;
    backward[j] = idx;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    const int j = forward[i];
    if (j >= 0 && backward[j] == static_cast<int>(i)) {
      matches.push_back({static_cast<int>(i), j, forward_dist[i]});
    }
  }
  std::sort(matches.begin(), matches.end(),
            [](const Match& x, const Match& y) { return x.distance < y.distance; });
  return matches;
}

// -------------------------------------------------------------- frame scoring

double laplacianVariance(const GrayImage& img, int step) {
  if (!img.valid() || img.width < 3 || img.height < 3) return 0.0;
  step = std::max(1, step);
  double sum = 0.0, sum_sq = 0.0;
  long count = 0;
  for (int y = 1; y < img.height - 1; y += step) {
    for (int x = 1; x < img.width - 1; x += step) {
      // 4-neighbour discrete Laplacian.
      const double lap = 4.0 * img.at(x, y) - img.at(x - 1, y) - img.at(x + 1, y) -
                         img.at(x, y - 1) - img.at(x, y + 1);
      sum += lap;
      sum_sq += lap * lap;
      ++count;
    }
  }
  if (count < 2) return 0.0;
  const double mean = sum / count;
  return std::max(0.0, sum_sq / count - mean * mean);
}

FrameQuality scoreFrame(const GrayImage& img, const FrameQualityThresholds& t) {
  FrameQuality q;
  if (!img.valid()) return q;

  double sum = 0.0, sum_sq = 0.0;
  long low = 0, high = 0, count = 0;
  for (int y = 0; y < img.height; y += 2) {
    for (int x = 0; x < img.width; x += 2) {
      const double v = img.at(x, y);
      sum += v;
      sum_sq += v * v;
      if (v <= 4.0) ++low;
      if (v >= 251.0) ++high;
      ++count;
    }
  }
  if (count == 0) return q;
  q.exposure = sum / count;
  q.contrast = std::sqrt(std::max(0.0, sum_sq / count - q.exposure * q.exposure));
  q.clipped_low = static_cast<double>(low) / count;
  q.clipped_high = static_cast<double>(high) / count;
  q.sharpness = laplacianVariance(img);

  q.acceptable = q.sharpness >= t.min_sharpness && q.exposure >= t.min_exposure &&
                 q.exposure <= t.max_exposure && q.clipped_low <= t.max_clipped &&
                 q.clipped_high <= t.max_clipped && q.contrast >= t.min_contrast;
  return q;
}

uint64_t differenceHash(const GrayImage& img) {
  // Classic dHash: resize to 9x8, compare horizontally adjacent pixels.
  const GrayBuffer small = resizeBilinear(img, 9, 8);
  uint64_t hash = 0;
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x, ++bit) {
      if (small.at(x, y) > small.at(x + 1, y)) hash |= (1ull << bit);
    }
  }
  return hash;
}

}  // namespace parallax
