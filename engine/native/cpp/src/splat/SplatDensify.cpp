#include "parallax/splat/SplatDensify.h"

#include <algorithm>
#include <cmath>

namespace parallax {
namespace {

/// A 32-bit xorshift. Small, deterministic and entirely sufficient for
/// jittering a child gaussian off its parent.
float nextNormal(uint32_t* state) {
  const auto next = [&]() {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x == 0u ? 1u : x;
    return *state;
  };
  // Box-Muller from two uniforms; one of the pair is enough here.
  const float u1 = std::max(1e-7f, static_cast<float>(next()) / 4294967296.f);
  const float u2 = static_cast<float>(next()) / 4294967296.f;
  return std::sqrt(-2.f * std::log(u1)) * std::cos(2.f * 3.14159265358979f * u2);
}

}  // namespace

SplatDensifyResult densifyAndPrune(GaussianCloud* cloud, const SplatDensifyStats& stats,
                                   const SplatTrainingConfig& config, float extent, int iteration,
                                   uint32_t* rng_state) {
  SplatDensifyResult result;
  const size_t before = cloud->gaussians.size();
  if (before == 0) return result;

  const float size_threshold = config.percent_dense * extent;
  std::vector<bool> keep(before, true);
  std::vector<Gaussian> additions;
  std::vector<int> addition_source;  // kept only for the counts

  const bool has_room = static_cast<int>(before) < config.max_gaussians;
  if (has_room) {
    for (size_t g = 0; g < before; ++g) {
      const int views_seen = g < stats.visible_count.size() ? stats.visible_count[g] : 0;
      if (views_seen <= 0) continue;
      const float mean_gradient = stats.screen_gradient[g] / static_cast<float>(views_seen);
      if (mean_gradient < config.densify_grad_threshold) continue;

      const Gaussian& parent = cloud->gaussians[g];
      const float largest = parent.log_scale.array().exp().maxCoeff();
      if (largest <= size_threshold) {
        // Under-reconstruction: too small for the detail it is being asked to
        // carry, so a copy is placed beside it and the two share the load.
        additions.push_back(parent);
        addition_source.push_back(static_cast<int>(g));
        ++result.cloned;
      } else {
        // Over-reconstruction: one wide gaussian standing in for structure,
        // replaced by two narrower ones drawn from its own distribution.
        const Vec3f scale = parent.log_scale.array().exp();
        const Eigen::Matrix3f rotation = parent.rotation.normalized().toRotationMatrix();
        for (int child = 0; child < 2; ++child) {
          Gaussian next = parent;
          const Vec3f sample(nextNormal(rng_state) * scale.x(), nextNormal(rng_state) * scale.y(),
                             nextNormal(rng_state) * scale.z());
          next.position = parent.position + rotation * sample;
          next.log_scale = parent.log_scale.array() - std::log(config.split_scale_divider);
          additions.push_back(std::move(next));
          addition_source.push_back(static_cast<int>(g));
        }
        keep[g] = false;
        ++result.split;
      }
    }
  }

  // Prune transparent gaussians always, and oversized ones once densification
  // has had time to settle -- early on a large gaussian is often correct.
  const bool prune_oversized = iteration > config.opacity_reset_interval;
  const float max_scale = config.prune_scale_fraction * extent;
  const auto prunable = [&](const Gaussian& gaussian) {
    if (gaussianOpacity(gaussian) < config.prune_opacity) return true;
    return prune_oversized && gaussian.log_scale.array().exp().maxCoeff() > max_scale;
  };

  std::vector<Gaussian> next;
  next.reserve(before + additions.size());
  result.source_index.reserve(before + additions.size());
  for (size_t g = 0; g < before; ++g) {
    if (!keep[g]) continue;
    if (prunable(cloud->gaussians[g])) {
      ++result.pruned;
      continue;
    }
    next.push_back(cloud->gaussians[g]);
    result.source_index.push_back(static_cast<int>(g));
  }
  for (Gaussian& gaussian : additions) {
    if (static_cast<int>(next.size()) >= config.max_gaussians) break;
    if (prunable(gaussian)) {
      ++result.pruned;
      continue;
    }
    next.push_back(std::move(gaussian));
    result.source_index.push_back(-1);
  }

  cloud->gaussians = std::move(next);
  return result;
}

}  // namespace parallax
