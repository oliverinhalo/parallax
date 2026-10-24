// Training a Gaussian splat scene by gradient descent.
//
// The loop here is the one from Kerbl et al. 2023: render a training view,
// score it, back-propagate, step Adam, and periodically add gaussians where
// the loss keeps pulling and remove the ones that have stopped earning their
// memory. Everything in this file is device-independent; the per-iteration
// rasterisation is delegated to a backend so the same schedule drives both the
// CPU reference and the Vulkan compute path.
#pragma once

#include "parallax/splat/SplatRaster.h"

#include <functional>
#include <memory>
#include <string>

namespace parallax {

/// One posed training image. Pixels are linear RGB in [0,1], row-major.
struct SplatTrainingView {
  SplatView view;
  std::vector<float> image;
};

struct SplatTrainingConfig {
  int iterations = 7000;

  /**
   * Learning rates, per parameter group.
   *
   * Position decays geometrically over training: early iterations have to move
   * gaussians across the scene, later ones only nudge them, and a rate large
   * enough for the first is large enough to destroy the second. The other
   * groups are stable enough to keep a fixed rate.
   *
   * `position_lr_*` are multiplied by the scene extent, so a tabletop object
   * and a whole room train at the same effective speed.
   */
  float position_lr_initial = 1.6e-4f;
  float position_lr_final = 1.6e-6f;
  float dc_lr = 2.5e-3f;
  /// Higher-order harmonics are fitted twenty times more slowly than the DC
  /// term; letting them move at the same rate makes them absorb detail that
  /// belongs in the geometry and the model turns to view-dependent mush.
  float sh_rest_lr_divider = 20.f;
  float opacity_lr = 0.05f;
  float scale_lr = 5e-3f;
  float rotation_lr = 1e-3f;

  float adam_beta1 = 0.9f;
  float adam_beta2 = 0.999f;
  float adam_epsilon = 1e-15f;

  /// Weight of the structural term in the photometric loss.
  float ssim_weight = 0.2f;

  /// Spherical-harmonic degree to finish at, and how often a band is added.
  int sh_degree = 3;
  int sh_increase_interval = 1000;

  int densify_from_iteration = 500;
  int densify_until_iteration = 5000;
  int densify_interval = 100;
  /// Average screen-space gradient above which a gaussian is split or cloned.
  float densify_grad_threshold = 2e-4f;
  /// A gaussian smaller than this fraction of the scene extent is cloned (it
  /// is under-covering); a larger one is split (it is over-reaching).
  float percent_dense = 0.01f;
  /// Children of a split get their scale divided by this.
  float split_scale_divider = 1.6f;

  /**
   * Opacity is driven to near zero every few thousand iterations.
   *
   * Densification can only add; without a counter-pressure the scene fills
   * with faint floaters that cost memory and add nothing. Resetting opacity
   * makes every gaussian re-earn it, and the prune that follows removes the
   * ones that cannot.
   */
  int opacity_reset_interval = 3000;
  float opacity_reset_value = 0.01f;

  float prune_opacity = 0.005f;
  /// Gaussians wider than this fraction of the scene extent are removed once
  /// densification has finished.
  float prune_scale_fraction = 0.1f;

  /// Hard ceiling, so a pathological capture cannot exhaust device memory.
  int max_gaussians = 600000;

  unsigned seed = 7u;

  std::string validate() const;
};

struct SplatTrainingProgress {
  int iteration = 0;
  int total_iterations = 0;
  int gaussian_count = 0;
  float loss = 0.f;
  /// Running mean of the loss over the last window, which is what actually
  /// tells you whether training is still improving.
  float smoothed_loss = 0.f;
};

/**
 * Where the per-iteration rasterisation happens.
 *
 * The CPU implementation is the reference and is what the tests exercise; the
 * Vulkan one runs the same equations as compute shaders. Splitting them here
 * rather than inside the loop keeps the schedule -- which is where the subtle
 * bugs live -- in one testable place.
 */
class SplatBackend {
 public:
  virtual ~SplatBackend() = default;
  virtual std::string name() const = 0;
  /// Render, score against `target`, and add the gradients into `gradients`.
  virtual float renderAndAccumulate(const GaussianCloud& cloud, const SplatView& view,
                                    const RasterOptions& options,
                                    const std::vector<float>& target, float ssim_weight,
                                    SplatGradients* gradients) = 0;
  virtual SplatImage render(const GaussianCloud& cloud, const SplatView& view,
                            const RasterOptions& options) = 0;
};

std::unique_ptr<SplatBackend> makeCpuSplatBackend();

/// Called with progress; returning false cancels training.
using SplatTrainingCallback = std::function<bool(const SplatTrainingProgress&)>;

class SplatTrainer {
 public:
  SplatTrainer(GaussianCloud initial, std::vector<SplatTrainingView> views,
               SplatTrainingConfig config, std::unique_ptr<SplatBackend> backend = nullptr);
  ~SplatTrainer();
  SplatTrainer(const SplatTrainer&) = delete;
  SplatTrainer& operator=(const SplatTrainer&) = delete;

  /// Run one iteration against one view. False once the schedule is finished.
  bool step();

  /// Run to completion. Returns false if `callback` cancelled it.
  bool train(const SplatTrainingCallback& callback = nullptr);

  int iteration() const;
  int shDegreeInUse() const;
  float lastLoss() const;
  float smoothedLoss() const;
  const GaussianCloud& cloud() const;
  /// Scene extent used to scale the position learning rate and the density
  /// thresholds: the radius of the training cameras about their centroid.
  float sceneExtent() const;

  /// Prune, sort by opacity and hand the cloud over. The trainer is spent.
  GaussianCloud finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace parallax
