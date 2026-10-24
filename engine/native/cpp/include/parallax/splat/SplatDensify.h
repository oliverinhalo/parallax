// Adaptive density control: adding gaussians where the loss keeps pulling and
// removing the ones that have stopped earning their memory.
//
// It lives apart from the trainer because both trainers need it and neither
// owns it: the schedule is identical whether the gradients came from the CPU
// reference or from the Vulkan compute path, and only the optimiser state it
// has to carry along differs.
#pragma once

#include "parallax/splat/GaussianCloud.h"
#include "parallax/splat/SplatTrainer.h"

#include <cstdint>
#include <vector>

namespace parallax {

/// What the backward passes recorded since the last densification.
struct SplatDensifyStats {
  /// Sum of the screen-space gradient magnitude over the views each gaussian
  /// appeared in.
  std::vector<float> screen_gradient;
  std::vector<int> visible_count;

  void reset(size_t count) {
    screen_gradient.assign(count, 0.f);
    visible_count.assign(count, 0);
  }
};

struct SplatDensifyResult {
  /**
   * For each gaussian in the new cloud, the index it had in the old one, or
   * -1 when it is newly added.
   *
   * This is the whole interface to the optimiser: a caller permutes its Adam
   * moments by this list and zeroes the new entries, and does not need to know
   * anything about how the decision was made. A new gaussian starting from
   * zero moments is deliberate -- it has no history, and inheriting its
   * parent's momentum would fling it across the scene on its first step.
   */
  std::vector<int> source_index;
  int cloned = 0;
  int split = 0;
  int pruned = 0;
};

/**
 * Clone, split and prune in place.
 *
 * `rng_state` is advanced, so a caller that keeps one state across the run
 * gets a deterministic sequence; splitting draws its children from the
 * parent's own distribution and so needs randomness.
 */
SplatDensifyResult densifyAndPrune(GaussianCloud* cloud, const SplatDensifyStats& stats,
                                   const SplatTrainingConfig& config, float extent, int iteration,
                                   uint32_t* rng_state);

}  // namespace parallax
