#include "parallax/fusion/IntegrationRange.h"

#include <algorithm>
#include <cmath>

namespace parallax {

namespace {
/// High enough to contain the surface, low enough to ignore samples that run to the horizon.
constexpr double kPercentile = 0.85;
constexpr float kMargin = 1.25f;
}  // namespace

float integrationDepthFrom(std::vector<float>* samples, float ceiling) {
  const float guard = std::max(kMinIntegrationDepthM, ceiling);
  if (samples == nullptr || samples->empty()) return guard;

  const size_t index =
      std::min(samples->size() - 1, static_cast<size_t>(samples->size() * kPercentile));
  // nth_element rather than a sort: the percentile is the only order statistic wanted, and
  // this runs over every eighth pixel of several depth maps.
  std::nth_element(samples->begin(), samples->begin() + index, samples->end());
  const float measured = (*samples)[index] * kMargin;
  if (!std::isfinite(measured)) return guard;
  return std::min(std::max(kMinIntegrationDepthM, measured), guard);
}

}  // namespace parallax
