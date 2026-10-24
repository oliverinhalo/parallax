#include "parallax/splat/SplatTrainer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace parallax {
namespace {

/// Deterministic generator, so the same capture always trains to the same
/// cloud. A reproducible result is worth more here than a slightly better one.
class TrainingRng {
 public:
  explicit TrainingRng(unsigned seed) : engine_(seed == 0u ? 1u : seed) {}
  float normal() { return normal_(engine_); }
  int index(int count) {
    return count <= 1 ? 0 : static_cast<int>(engine_() % static_cast<uint32_t>(count));
  }
  void shuffle(std::vector<int>* values) {
    std::shuffle(values->begin(), values->end(), engine_);
  }

 private:
  std::mt19937 engine_;
  std::normal_distribution<float> normal_{0.f, 1.f};
};

/// Adam moments for one parameter array. Growing the array leaves the new
/// entries at zero, which is the same state a fresh parameter would start in.
struct AdamState {
  std::vector<float> first;
  std::vector<float> second;

  void resize(size_t n) {
    first.resize(n, 0.f);
    second.resize(n, 0.f);
  }
  void append(size_t n) {
    first.insert(first.end(), n, 0.f);
    second.insert(second.end(), n, 0.f);
  }
};

struct AdamSettings {
  float beta1;
  float beta2;
  float epsilon;
  /// Bias-correction denominators, recomputed each step.
  float correction1;
  float correction2;
};

float adamDelta(AdamState* state, size_t i, float gradient, float learning_rate,
                const AdamSettings& s) {
  float& m = state->first[i];
  float& v = state->second[i];
  m = s.beta1 * m + (1.f - s.beta1) * gradient;
  v = s.beta2 * v + (1.f - s.beta2) * gradient * gradient;
  const float m_hat = m / s.correction1;
  const float v_hat = v / s.correction2;
  return -learning_rate * m_hat / (std::sqrt(v_hat) + s.epsilon);
}

float sigmoidInverse(float value) {
  const float clamped = std::clamp(value, 1e-6f, 1.f - 1e-6f);
  return std::log(clamped / (1.f - clamped));
}

/// Radius of the training cameras about their centroid; the scene's natural
/// unit of length and what the reference calls `cameras_extent`.
float cameraExtent(const std::vector<SplatTrainingView>& views) {
  if (views.empty()) return 1.f;
  Vec3f centroid = Vec3f::Zero();
  for (const SplatTrainingView& v : views) centroid += v.view.camera_from_world.center().cast<float>();
  centroid /= static_cast<float>(views.size());
  float furthest = 0.f;
  for (const SplatTrainingView& v : views) {
    furthest = std::max(furthest,
                        (v.view.camera_from_world.center().cast<float>() - centroid).norm());
  }
  return std::max(furthest * 1.1f, 1e-3f);
}

/**
 * The reference backend.
 *
 * It is the definition of what the shaders are supposed to compute, and it is
 * what the tests run against. It is not a shipping path for real captures:
 * ADR-009 gates splat training on Vulkan precisely because this arithmetic is
 * orders of magnitude too slow on a phone CPU.
 */
class CpuSplatBackend final : public SplatBackend {
 public:
  std::string name() const override { return "cpu"; }

  SplatImage render(const GaussianCloud& cloud, const SplatView& view,
                    const RasterOptions& options) override {
    return renderSplats(cloud, view, options);
  }

  float renderAndAccumulate(const GaussianCloud& cloud, const SplatView& view,
                            const RasterOptions& options, const std::vector<float>& target,
                            float ssim_weight, SplatGradients* gradients) override {
    const std::vector<ProjectedSplat> projected = projectSplats(cloud, view, options);
    const SplatImage image = rasterizeSplats(projected, view, options);
    std::vector<float> dL_drgb;
    const float loss = photometricLoss(image, target, ssim_weight, &dL_drgb);
    rasterizeSplatsBackward(cloud, view, options, projected, image, dL_drgb, gradients);
    return loss;
  }
};

}  // namespace

std::string SplatTrainingConfig::validate() const {
  if (iterations <= 0) return "iterations must be positive";
  if (sh_degree < 0 || sh_degree > 3) return "sh_degree must be between 0 and 3";
  if (densify_interval <= 0) return "densify_interval must be positive";
  if (split_scale_divider <= 1.f) return "split_scale_divider must be greater than one";
  if (max_gaussians <= 0) return "max_gaussians must be positive";
  if (ssim_weight < 0.f || ssim_weight > 1.f) return "ssim_weight must be in [0,1]";
  return {};
}

std::unique_ptr<SplatBackend> makeCpuSplatBackend() {
  return std::make_unique<CpuSplatBackend>();
}

// ----------------------------------------------------------------- state ---

struct SplatTrainer::Impl {
  GaussianCloud cloud;
  std::vector<SplatTrainingView> views;
  SplatTrainingConfig config;
  std::unique_ptr<SplatBackend> backend;
  TrainingRng rng;

  int iteration = 0;
  float extent = 1.f;
  float last_loss = 0.f;
  float smoothed_loss = 0.f;
  int rest_stride = 0;

  AdamState adam_position, adam_log_scale, adam_rotation, adam_opacity, adam_dc, adam_sh_rest;

  /// Reused each iteration; `resize` zeroes it, so it never carries over.
  SplatGradients gradients;

  /// Running densification statistics, reset after every densify pass.
  std::vector<float> accumulated_gradient;
  std::vector<int> accumulated_views;

  /// Shuffled view order, reshuffled each epoch so no view is starved.
  std::vector<int> order;
  size_t order_cursor = 0;

  Impl(GaussianCloud initial, std::vector<SplatTrainingView> training_views,
       SplatTrainingConfig cfg, std::unique_ptr<SplatBackend> chosen)
      : cloud(std::move(initial)),
        views(std::move(training_views)),
        config(cfg),
        backend(chosen != nullptr ? std::move(chosen) : makeCpuSplatBackend()),
        rng(cfg.seed) {
    extent = cameraExtent(views);
    rest_stride = cloud.gaussians.empty() ? 0 : static_cast<int>(cloud.gaussians.front().sh_rest.size());
    resizeState();
    order.resize(views.size());
    std::iota(order.begin(), order.end(), 0);
    rng.shuffle(&order);
  }

  size_t count() const { return cloud.gaussians.size(); }

  void resizeState() {
    const size_t n = count();
    adam_position.resize(n * 3);
    adam_log_scale.resize(n * 3);
    adam_rotation.resize(n * 4);
    adam_opacity.resize(n);
    adam_dc.resize(n * 3);
    adam_sh_rest.resize(n * static_cast<size_t>(rest_stride));
    accumulated_gradient.resize(n, 0.f);
    accumulated_views.resize(n, 0);
  }

  int shDegreeInUse() const {
    const int unlocked = config.sh_increase_interval > 0
                             ? iteration / config.sh_increase_interval
                             : config.sh_degree;
    return std::min(config.sh_degree, std::max(0, unlocked));
  }

  float positionLearningRate() const {
    // Geometric decay from initial to final across the whole schedule, the
    // same shape as the reference's exponential scheduler.
    const float t = config.iterations <= 1
                        ? 1.f
                        : static_cast<float>(iteration) / static_cast<float>(config.iterations - 1);
    const float ratio = config.position_lr_final / std::max(config.position_lr_initial, 1e-30f);
    return config.position_lr_initial * std::pow(ratio, std::clamp(t, 0.f, 1.f)) * extent;
  }

  const SplatTrainingView& nextView() {
    if (order_cursor >= order.size()) {
      rng.shuffle(&order);
      order_cursor = 0;
    }
    return views[static_cast<size_t>(order[order_cursor++])];
  }

  void applyGradients(const SplatGradients& gradients) {
    AdamSettings settings{config.adam_beta1, config.adam_beta2, config.adam_epsilon, 1.f, 1.f};
    const int t = iteration + 1;
    settings.correction1 = 1.f - std::pow(config.adam_beta1, static_cast<float>(t));
    settings.correction2 = 1.f - std::pow(config.adam_beta2, static_cast<float>(t));

    const float position_lr = positionLearningRate();
    const float sh_rest_lr = config.dc_lr / std::max(1.f, config.sh_rest_lr_divider);

    for (size_t g = 0; g < count(); ++g) {
      Gaussian& gaussian = cloud.gaussians[g];
      for (int a = 0; a < 3; ++a) {
        gaussian.position[a] +=
            adamDelta(&adam_position, g * 3 + a, gradients.position[g][a], position_lr, settings);
        gaussian.log_scale[a] += adamDelta(&adam_log_scale, g * 3 + a, gradients.log_scale[g][a],
                                           config.scale_lr, settings);
        gaussian.dc[a] += adamDelta(&adam_dc, g * 3 + a, gradients.dc[g][a], config.dc_lr, settings);
      }
      gaussian.opacity_logit += adamDelta(&adam_opacity, g, gradients.opacity_logit[g],
                                          config.opacity_lr, settings);

      float* quaternion[4] = {&gaussian.rotation.w(), &gaussian.rotation.x(),
                              &gaussian.rotation.y(), &gaussian.rotation.z()};
      for (int a = 0; a < 4; ++a) {
        *quaternion[a] += adamDelta(&adam_rotation, g * 4 + a, gradients.rotation[g][a],
                                    config.rotation_lr, settings);
      }
      // Re-normalising every step keeps the Adam moments meaningful; letting
      // the norm drift makes the effective learning rate drift with it.
      if (gaussian.rotation.norm() > 1e-6f) gaussian.rotation.normalize();
      else gaussian.rotation = Eigen::Quaternionf::Identity();

      for (int k = 0; k < rest_stride; ++k) {
        const size_t i = g * static_cast<size_t>(rest_stride) + static_cast<size_t>(k);
        gaussian.sh_rest[static_cast<size_t>(k)] +=
            adamDelta(&adam_sh_rest, i, gradients.sh_rest[i], sh_rest_lr, settings);
      }
    }
  }

  void accumulateStatistics(const SplatGradients& gradients) {
    for (size_t g = 0; g < count(); ++g) {
      accumulated_gradient[g] += gradients.screen_gradient[g];
      accumulated_views[g] += gradients.visible_count[g];
    }
  }

  /// Remove the gaussians `keep` marks false, carrying the optimiser state and
  /// the densification statistics with them.
  void compact(const std::vector<bool>& keep) {
    const size_t n = count();
    size_t write = 0;
    for (size_t read = 0; read < n; ++read) {
      if (!keep[read]) continue;
      if (write != read) {
        cloud.gaussians[write] = cloud.gaussians[read];
        accumulated_gradient[write] = accumulated_gradient[read];
        accumulated_views[write] = accumulated_views[read];
        moveState(read, write);
      }
      ++write;
    }
    cloud.gaussians.resize(write);
    accumulated_gradient.resize(write);
    accumulated_views.resize(write);
    truncateState(write);
  }

  void moveState(size_t from, size_t to) {
    const auto move = [&](AdamState* state, size_t stride) {
      for (size_t k = 0; k < stride; ++k) {
        state->first[to * stride + k] = state->first[from * stride + k];
        state->second[to * stride + k] = state->second[from * stride + k];
      }
    };
    move(&adam_position, 3);
    move(&adam_log_scale, 3);
    move(&adam_rotation, 4);
    move(&adam_opacity, 1);
    move(&adam_dc, 3);
    if (rest_stride > 0) move(&adam_sh_rest, static_cast<size_t>(rest_stride));
  }

  void truncateState(size_t n) {
    adam_position.resize(n * 3);
    adam_log_scale.resize(n * 3);
    adam_rotation.resize(n * 4);
    adam_opacity.resize(n);
    adam_dc.resize(n * 3);
    adam_sh_rest.resize(n * static_cast<size_t>(rest_stride));
  }

  void appendState(size_t added) {
    adam_position.append(added * 3);
    adam_log_scale.append(added * 3);
    adam_rotation.append(added * 4);
    adam_opacity.append(added);
    adam_dc.append(added * 3);
    adam_sh_rest.append(added * static_cast<size_t>(rest_stride));
    accumulated_gradient.insert(accumulated_gradient.end(), added, 0.f);
    accumulated_views.insert(accumulated_views.end(), added, 0);
  }

  /// Mean screen-space gradient per gaussian since the last densify pass.
  float meanGradient(size_t g) const {
    const int views_seen = accumulated_views[g];
    return views_seen > 0 ? accumulated_gradient[g] / static_cast<float>(views_seen) : 0.f;
  }

  void densifyAndPrune() {
    const size_t before = count();
    const float size_threshold = config.percent_dense * extent;
    const int room = config.max_gaussians - static_cast<int>(before);

    std::vector<Gaussian> clones;
    std::vector<size_t> split_sources;
    if (room > 0) {
      for (size_t g = 0; g < before; ++g) {
        if (meanGradient(g) < config.densify_grad_threshold) continue;
        const float largest = cloud.gaussians[g].log_scale.array().exp().maxCoeff();
        if (largest <= size_threshold) {
          // Under-reconstruction: the gaussian is too small for the detail it
          // is being asked to carry, so a copy is placed beside it.
          clones.push_back(cloud.gaussians[g]);
        } else {
          split_sources.push_back(g);
        }
      }
    }

    // Splitting replaces one wide gaussian with two narrower ones drawn from
    // its own distribution, which is what lets a blurry blob resolve.
    std::vector<Gaussian> children;
    std::vector<bool> keep(before, true);
    for (size_t g : split_sources) {
      const Gaussian& parent = cloud.gaussians[g];
      const Vec3f scale = parent.log_scale.array().exp();
      const Eigen::Matrix3f rotation = parent.rotation.normalized().toRotationMatrix();
      for (int child = 0; child < 2; ++child) {
        Gaussian next = parent;
        const Vec3f sample(rng.normal() * scale.x(), rng.normal() * scale.y(),
                           rng.normal() * scale.z());
        next.position = parent.position + rotation * sample;
        next.log_scale = parent.log_scale.array() - std::log(config.split_scale_divider);
        children.push_back(std::move(next));
      }
      keep[g] = false;
    }

    size_t added = 0;
    for (std::vector<Gaussian>* batch : {&clones, &children}) {
      for (Gaussian& g : *batch) {
        if (static_cast<int>(count()) >= config.max_gaussians) break;
        cloud.gaussians.push_back(std::move(g));
        ++added;
      }
    }
    keep.resize(cloud.gaussians.size(), true);
    appendState(added);

    // Prune: transparent gaussians always, oversized ones once densification
    // has had time to settle (early on a large gaussian is often correct).
    const bool prune_oversized = iteration > config.opacity_reset_interval;
    const float max_scale = config.prune_scale_fraction * extent;
    for (size_t g = 0; g < cloud.gaussians.size(); ++g) {
      if (!keep[g]) continue;
      const Gaussian& gaussian = cloud.gaussians[g];
      if (gaussianOpacity(gaussian) < config.prune_opacity) {
        keep[g] = false;
      } else if (prune_oversized && gaussian.log_scale.array().exp().maxCoeff() > max_scale) {
        keep[g] = false;
      }
    }
    compact(keep);

    std::fill(accumulated_gradient.begin(), accumulated_gradient.end(), 0.f);
    std::fill(accumulated_views.begin(), accumulated_views.end(), 0);
  }

  void resetOpacity() {
    const float ceiling = sigmoidInverse(config.opacity_reset_value);
    for (size_t g = 0; g < count(); ++g) {
      Gaussian& gaussian = cloud.gaussians[g];
      gaussian.opacity_logit = std::min(gaussian.opacity_logit, ceiling);
      adam_opacity.first[g] = 0.f;
      adam_opacity.second[g] = 0.f;
    }
  }
};

SplatTrainer::SplatTrainer(GaussianCloud initial, std::vector<SplatTrainingView> views,
                           SplatTrainingConfig config, std::unique_ptr<SplatBackend> backend)
    : impl_(std::make_unique<Impl>(std::move(initial), std::move(views), config,
                                   std::move(backend))) {}

SplatTrainer::~SplatTrainer() = default;

int SplatTrainer::iteration() const { return impl_->iteration; }
int SplatTrainer::shDegreeInUse() const { return impl_->shDegreeInUse(); }
float SplatTrainer::lastLoss() const { return impl_->last_loss; }
float SplatTrainer::smoothedLoss() const { return impl_->smoothed_loss; }
const GaussianCloud& SplatTrainer::cloud() const { return impl_->cloud; }
float SplatTrainer::sceneExtent() const { return impl_->extent; }

bool SplatTrainer::step() {
  Impl& impl = *impl_;
  if (impl.iteration >= impl.config.iterations) return false;
  if (impl.views.empty() || impl.cloud.empty()) {
    impl.iteration = impl.config.iterations;
    return false;
  }

  RasterOptions options;
  options.sh_degree = impl.shDegreeInUse();

  const SplatTrainingView& view = impl.nextView();
  impl.gradients.resize(impl.count(), impl.rest_stride);
  impl.last_loss = impl.backend->renderAndAccumulate(
      impl.cloud, view.view, options, view.image, impl.config.ssim_weight, &impl.gradients);
  // An exponential mean over roughly the last hundred iterations: enough to
  // see through the view-to-view variation without lagging a real change.
  impl.smoothed_loss = impl.iteration == 0
                           ? impl.last_loss
                           : 0.99f * impl.smoothed_loss + 0.01f * impl.last_loss;

  impl.accumulateStatistics(impl.gradients);
  impl.applyGradients(impl.gradients);
  ++impl.iteration;

  const SplatTrainingConfig& config = impl.config;
  if (impl.iteration >= config.densify_from_iteration &&
      impl.iteration <= config.densify_until_iteration &&
      impl.iteration % config.densify_interval == 0) {
    impl.densifyAndPrune();
  }
  if (config.opacity_reset_interval > 0 && impl.iteration % config.opacity_reset_interval == 0 &&
      impl.iteration < config.densify_until_iteration) {
    impl.resetOpacity();
  }
  return true;
}

bool SplatTrainer::train(const SplatTrainingCallback& callback) {
  while (step()) {
    if (callback == nullptr) continue;
    SplatTrainingProgress progress;
    progress.iteration = impl_->iteration;
    progress.total_iterations = impl_->config.iterations;
    progress.gaussian_count = static_cast<int>(impl_->count());
    progress.loss = impl_->last_loss;
    progress.smoothed_loss = impl_->smoothed_loss;
    if (!callback(progress)) return false;
  }
  return true;
}

GaussianCloud SplatTrainer::finish() {
  impl_->cloud.prune(impl_->config.prune_opacity);
  impl_->cloud.sortByOpacity();
  impl_->cloud.sh_degree = impl_->config.sh_degree;
  return std::move(impl_->cloud);
}

}  // namespace parallax
