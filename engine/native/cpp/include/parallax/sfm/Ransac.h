// Generic RANSAC driver with an adaptive stopping rule.
//
// Kept header-only and model-agnostic so the essential matrix, homography and
// PnP estimators all share one tested implementation of the sampling and
// consensus loop rather than each growing their own.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/testing/SyntheticScene.h"  // Rng

#include <algorithm>
#include <functional>

namespace parallax {

struct RansacOptions {
  double inlier_threshold = 2.0;   ///< model-specific units (usually pixels)
  int max_iterations = 2000;
  int min_iterations = 30;
  double confidence = 0.9995;      ///< probability of finding an all-inlier sample
  uint64_t seed = 0x5EED5EED5EED5EEDull;
};

template <typename Model>
struct RansacResult {
  Model model{};
  std::vector<uint8_t> inliers;    ///< per-datum mask, 1 == inlier
  int inlier_count = 0;
  int iterations = 0;
  bool success = false;

  double inlierRatio(size_t total) const {
    return total ? static_cast<double>(inlier_count) / total : 0.0;
  }
};

/// Draw `k` distinct indices from [0, n) without replacement.
inline bool sampleDistinct(testing::Rng& rng, int n, int k, std::vector<int>* out) {
  if (n < k) return false;
  out->clear();
  // Partial Fisher-Yates over a small scratch set: for the sample sizes RANSAC
  // uses (4..8) rejection sampling is cheaper than shuffling the whole range.
  for (int attempt = 0; attempt < 100 && static_cast<int>(out->size()) < k; ++attempt) {
    const int idx = static_cast<int>(rng.uniform() * n);
    if (idx < 0 || idx >= n) continue;
    if (std::find(out->begin(), out->end(), idx) == out->end()) out->push_back(idx);
  }
  return static_cast<int>(out->size()) == k;
}

/**
 * Run RANSAC.
 *
 * @param count          number of data points
 * @param sample_size    minimal sample size for the model
 * @param fit            builds candidate models from a minimal sample; may emit
 *                       several (the essential matrix yields up to four poses)
 * @param residual       squared-or-absolute error of one datum under a model,
 *                       compared against options.inlier_threshold
 * @param refit          optional non-minimal refit over the final inlier set
 */
template <typename Model>
RansacResult<Model> runRansac(
    int count, int sample_size, const RansacOptions& options,
    const std::function<bool(const std::vector<int>&, std::vector<Model>*)>& fit,
    const std::function<double(const Model&, int)>& residual,
    const std::function<bool(const std::vector<uint8_t>&, Model*)>& refit = nullptr) {
  RansacResult<Model> best;
  if (count < sample_size || sample_size <= 0) return best;

  testing::Rng rng(options.seed);
  std::vector<int> sample;
  std::vector<Model> candidates;
  std::vector<uint8_t> mask(count, 0);

  int max_iterations = options.max_iterations;
  for (int iter = 0; iter < max_iterations; ++iter) {
    best.iterations = iter + 1;
    if (!sampleDistinct(rng, count, sample_size, &sample)) break;

    candidates.clear();
    if (!fit(sample, &candidates)) continue;

    for (const Model& candidate : candidates) {
      int inliers = 0;
      for (int i = 0; i < count; ++i) {
        const double r = residual(candidate, i);
        const uint8_t ok = (r <= options.inlier_threshold) ? 1 : 0;
        mask[i] = ok;
        inliers += ok;
      }
      if (inliers > best.inlier_count) {
        best.inlier_count = inliers;
        best.model = candidate;
        best.inliers = mask;
        best.success = inliers >= sample_size;

        // Adaptive stopping: once a large consensus set exists, the chance that
        // more sampling helps collapses. Recompute the required iteration count
        // from the observed inlier ratio (Fischler & Bolles).
        const double w = static_cast<double>(inliers) / count;
        if (w > 0.0 && w < 1.0) {
          const double pNoOutliers = 1.0 - std::pow(w, sample_size);
          if (pNoOutliers > 1e-12 && pNoOutliers < 1.0) {
            const double needed =
                std::log(1.0 - options.confidence) / std::log(pNoOutliers);
            if (std::isfinite(needed)) {
              max_iterations = std::min<int>(
                  options.max_iterations,
                  std::max(options.min_iterations, static_cast<int>(std::ceil(needed))));
            }
          }
        } else if (w >= 1.0) {
          max_iterations = std::min(max_iterations, iter + 1);
        }
      }
    }
  }

  // Non-minimal refit over the consensus set, then one more consensus pass.
  if (best.success && refit) {
    Model refined = best.model;
    if (refit(best.inliers, &refined)) {
      int inliers = 0;
      for (int i = 0; i < count; ++i) {
        const uint8_t ok = (residual(refined, i) <= options.inlier_threshold) ? 1 : 0;
        mask[i] = ok;
        inliers += ok;
      }
      // Only accept the refit if it did not lose consensus.
      if (inliers >= best.inlier_count) {
        best.model = refined;
        best.inliers = mask;
        best.inlier_count = inliers;
      }
    }
  }
  return best;
}

}  // namespace parallax
