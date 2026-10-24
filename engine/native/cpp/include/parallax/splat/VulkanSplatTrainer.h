// Gaussian splat training on the GPU.
//
// The schedule, the densification and the mathematics are all shared with the
// CPU reference in SplatTrainer/SplatRaster; what differs is that the whole
// per-iteration inner loop runs as Vulkan compute, with the model living in
// device buffers from the first iteration to the last.
#pragma once

#include "parallax/splat/SplatTrainer.h"

#include <string>

namespace parallax {

struct SplatGpuAvailability {
  bool available = false;
  /// The GPU's own name, for the settings screen and the processing log.
  std::string device_name;
  /// Why not, when it is not. Phrased for a person to read.
  std::string reason;
};

/// Probes for a usable compute device. Cheap enough to call from a settings
/// screen; it creates and destroys an instance and nothing more.
SplatGpuAvailability splatGpuAvailability();

/**
 * Train `cloud` in place on the GPU.
 *
 * Returns false and sets `error` if the device cannot do it, which is an
 * ordinary outcome rather than a failure: the caller falls back to the CPU
 * trainer. `callback` returning false cancels, and the partially trained
 * cloud is still written back.
 */
bool trainSplatsOnGpu(GaussianCloud* cloud, const std::vector<SplatTrainingView>& views,
                      const SplatTrainingConfig& config, const SplatTrainingCallback& callback,
                      std::string* error);

}  // namespace parallax
