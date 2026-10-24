#include "parallax/splat/VulkanSplatTrainer.h"

#include "parallax/splat/SplatDensify.h"

#if !defined(__ANDROID__) || !defined(PARALLAX_HAVE_SPLAT_SHADERS)

namespace parallax {

SplatGpuAvailability splatGpuAvailability() {
  SplatGpuAvailability availability;
  availability.reason = "GPU splat training is only built into the Android library";
  return availability;
}

bool trainSplatsOnGpu(GaussianCloud*, const std::vector<SplatTrainingView>&,
                      const SplatTrainingConfig&, const SplatTrainingCallback&,
                      std::string* error) {
  if (error != nullptr) *error = "GPU splat training is not built into this binary";
  return false;
}

}  // namespace parallax

#else

#include "SplatShaders.h"
#include "VulkanContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace parallax {
namespace {

using vk::Binding;
using vk::Buffer;
using vk::Context;
using vk::Pipeline;

constexpr uint32_t kTileSize = 16;
constexpr uint32_t kWorkgroup = 256;
constexpr uint32_t kScanBlock = 512;
constexpr uint32_t kRadixDigits = 16;
constexpr uint32_t kRadixPasses = 8;  ///< 8 passes of 4 bits covers 32-bit keys
constexpr uint32_t kSsimPlanes = 5;

/// Reserved tile index that padding in the sort key array maps to, so the
/// pairs that are not really there cannot land in a tile that gets rendered.
constexpr uint32_t kMaxTiles = (1u << 14) - 1u;

/**
 * Ceiling on the model size for the GPU path.
 *
 * It is what keeps the prefix sums to two levels: a quarter of a million
 * gaussians is 512 blocks of 512, exactly one recursion. It is also about as
 * much as a phone can hold -- at degree three that is already sixty megabytes
 * of parameters before the gradients and the optimiser moments.
 */
constexpr int kMaxGaussiansOnGpu = 262144;

/// Tiles a splat is expected to touch, used to size the pair array. Checked
/// against the real count at every densification and grown if it was wrong.
constexpr uint32_t kPairsPerSplatEstimate = 6;

uint32_t groupsFor(uint32_t items, uint32_t group) { return (items + group - 1) / group; }

bool failWith(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}
uint32_t roundUp(uint32_t value, uint32_t multiple) {
  return groupsFor(value, multiple) * multiple;
}

/// Mirrors the ViewParams uniform block in params.glsl. std140 puts a mat4 at
/// 64 bytes and every vec4 on a 16-byte boundary, which this layout satisfies
/// by using only those two shapes.
struct ViewParamsBlock {
  float camera_from_world[16];
  float camera_center[4];
  float intrinsics[4];
  int32_t size[4];
  float options[4];
  int32_t tiles[4];
  float background[4];
  float depth_range[4];
};

struct ScanPush {
  uint32_t count;
};
struct RadixPush {
  uint32_t count;
  uint32_t shift;
  uint32_t block_count;
};
struct AdamPush {
  float lr_position;
  float lr_scale;
  float lr_rotation;
  float lr_opacity;
  float lr_dc;
  float lr_sh_rest;
  float beta1;
  float beta2;
  float epsilon;
  float correction1;
  float correction2;
  uint32_t count;
  uint32_t rest_stride;
  uint32_t moment_base_position;
  uint32_t moment_base_log_scale;
  uint32_t moment_base_rotation;
  uint32_t moment_base_opacity;
  uint32_t moment_base_dc;
  uint32_t moment_base_sh_rest;
};
struct LossPush {
  uint32_t value_count;
  float ssim_weight;
  uint32_t total_slot;
};
struct PreparePush {
  uint32_t pixel_count;
  uint32_t channel;
};
struct BlurPush {
  uint32_t width;
  uint32_t height;
  uint32_t plane_count;
  uint32_t vertical;
};
struct SsimTermsPush {
  uint32_t pixel_count;
  float scale;
  uint32_t total_slot;
  uint32_t want_gradient;
};
struct SsimGradientPush {
  uint32_t pixel_count;
  uint32_t channel;
  float ssim_weight;
};

/// Flat, per-group views of the cloud, which is the layout the shaders read.
struct HostParameters {
  std::vector<float> position, log_scale, rotation, opacity, dc, sh_rest;

  void resize(size_t capacity, size_t rest_stride) {
    position.assign(capacity * 3, 0.f);
    log_scale.assign(capacity * 3, 0.f);
    rotation.assign(capacity * 4, 0.f);
    opacity.assign(capacity, 0.f);
    dc.assign(capacity * 3, 0.f);
    sh_rest.assign(capacity * rest_stride, 0.f);
  }

  void fromCloud(const GaussianCloud& cloud, size_t rest_stride) {
    for (size_t g = 0; g < cloud.gaussians.size(); ++g) {
      const Gaussian& gaussian = cloud.gaussians[g];
      for (int a = 0; a < 3; ++a) {
        position[g * 3 + a] = gaussian.position[a];
        log_scale[g * 3 + a] = gaussian.log_scale[a];
        dc[g * 3 + a] = gaussian.dc[static_cast<size_t>(a)];
      }
      rotation[g * 4 + 0] = gaussian.rotation.w();
      rotation[g * 4 + 1] = gaussian.rotation.x();
      rotation[g * 4 + 2] = gaussian.rotation.y();
      rotation[g * 4 + 3] = gaussian.rotation.z();
      opacity[g] = gaussian.opacity_logit;
      for (size_t k = 0; k < rest_stride && k < gaussian.sh_rest.size(); ++k) {
        sh_rest[g * rest_stride + k] = gaussian.sh_rest[k];
      }
    }
  }

  void toCloud(GaussianCloud* cloud, size_t rest_stride) const {
    for (size_t g = 0; g < cloud->gaussians.size(); ++g) {
      Gaussian& gaussian = cloud->gaussians[g];
      for (int a = 0; a < 3; ++a) {
        gaussian.position[a] = position[g * 3 + a];
        gaussian.log_scale[a] = log_scale[g * 3 + a];
        gaussian.dc[static_cast<size_t>(a)] = dc[g * 3 + a];
      }
      gaussian.rotation = Eigen::Quaternionf(rotation[g * 4 + 0], rotation[g * 4 + 1],
                                             rotation[g * 4 + 2], rotation[g * 4 + 3]);
      if (gaussian.rotation.norm() > 1e-6f) gaussian.rotation.normalize();
      else gaussian.rotation = Eigen::Quaternionf::Identity();
      gaussian.opacity_logit = opacity[g];
      gaussian.sh_rest.assign(rest_stride, 0.f);
      for (size_t k = 0; k < rest_stride; ++k) gaussian.sh_rest[k] = sh_rest[g * rest_stride + k];
    }
  }
};

float cameraExtentOf(const std::vector<SplatTrainingView>& views) {
  if (views.empty()) return 1.f;
  Vec3f centroid = Vec3f::Zero();
  for (const SplatTrainingView& v : views) {
    centroid += v.view.camera_from_world.center().cast<float>();
  }
  centroid /= static_cast<float>(views.size());
  float furthest = 0.f;
  for (const SplatTrainingView& v : views) {
    furthest = std::max(furthest,
                        (v.view.camera_from_world.center().cast<float>() - centroid).norm());
  }
  return std::max(furthest * 1.1f, 1e-3f);
}

}  // namespace

// ---------------------------------------------------------------- trainer ---

namespace {

class GpuTrainer {
 public:
  GpuTrainer(std::unique_ptr<Context> context, const SplatTrainingConfig& config)
      : context_(std::move(context)), config_(config) {}

  ~GpuTrainer() {
    for (Pipeline* pipeline : owned_pipelines_) context_->destroyPipeline(pipeline);
  }

  bool setUp(const GaussianCloud& cloud, const std::vector<SplatTrainingView>& views,
             std::string* error);
  bool run(GaussianCloud* cloud, const std::vector<SplatTrainingView>& views,
           const SplatTrainingCallback& callback, bool* cancelled, std::string* error);

 private:
  bool allocate(std::string* error);
  bool buildPipelines(std::string* error);
  bool growPairCapacity(uint32_t wanted, std::string* error);
  void recordScan(const Pipeline& block, const Pipeline& top, const Pipeline& add,
                  uint32_t count, uint32_t level1_count);
  void recordIteration(const SplatTrainingView& view);
  bool uploadParameters();
  bool downloadParameters();
  bool densify(GaussianCloud* cloud, int iteration, std::string* error);
  float positionLearningRate(int iteration) const;

  std::unique_ptr<Context> context_;
  SplatTrainingConfig config_;

  uint32_t capacity_ = 0;
  uint32_t active_ = 0;
  uint32_t rest_stride_ = 0;
  uint32_t pair_capacity_ = 0;
  uint32_t max_pixels_ = 0;
  uint32_t tile_cols_ = 0;
  uint32_t tile_rows_ = 0;
  float extent_ = 1.f;
  uint32_t densify_rng_ = 1u;

  HostParameters host_;
  std::vector<float> host_statistics_;
  std::vector<uint32_t> host_visible_counts_;
  std::vector<uint32_t> host_tile_counts_;
  std::vector<float> host_moments_;

  Buffer params_, positions_, log_scales_, rotations_, opacities_, dc_, sh_rest_;
  Buffer grad_position_, grad_log_scale_, grad_rotation_, grad_opacity_, grad_dc_, grad_sh_rest_;
  Buffer moments_, statistics_, visible_counts_;
  Buffer proj_, meta_, tile_counts_, pair_offset_, scan_sums_, scan_scanned_, scan_sums_top_;
  Buffer pair_key_a_, pair_key_b_, pair_value_a_, pair_value_b_;
  Buffer histogram_, histogram_scanned_, histogram_sums_, histogram_sums_scanned_,
      histogram_sums_top_;
  Buffer tile_range_, screen_gradient_;
  Buffer rendered_, transmittance_, contributors_, dL_drgb_, target_;
  Buffer planes_, scratch_, blurred_, terms_, blurred_terms_, totals_;

  Pipeline project_, pair_write_, tile_ranges_, forward_, backward_, backward_params_, adam_;
  Pipeline scan_pairs_block_, scan_pairs_top_, scan_pairs_add_;
  Pipeline scan_hist_block_, scan_hist_top_, scan_hist_add_;
  Pipeline radix_hist_a_, radix_hist_b_, radix_scatter_ab_, radix_scatter_ba_;
  Pipeline loss_l1_, ssim_prepare_, ssim_terms_, ssim_gradient_;
  Pipeline blur_planes_h_, blur_planes_v_, blur_terms_h_, blur_terms_v_;
  std::vector<Pipeline*> owned_pipelines_;
};

bool GpuTrainer::setUp(const GaussianCloud& cloud, const std::vector<SplatTrainingView>& views,
                       std::string* error) {
  if (cloud.empty() || views.empty()) {
    if (error != nullptr) *error = "nothing to train";
    return false;
  }
  rest_stride_ = static_cast<uint32_t>(cloud.gaussians.front().sh_rest.size());
  capacity_ = static_cast<uint32_t>(
      std::min<int>(std::max<int>(config_.max_gaussians, static_cast<int>(cloud.size())),
                    kMaxGaussiansOnGpu));
  if (cloud.size() > capacity_) {
    if (error != nullptr) {
      *error = "the scene already has more gaussians than the GPU path allows";
    }
    return false;
  }
  active_ = static_cast<uint32_t>(cloud.size());
  extent_ = cameraExtentOf(views);
  densify_rng_ = config_.seed == 0u ? 1u : config_.seed;

  uint32_t max_cols = 0, max_rows = 0;
  for (const SplatTrainingView& view : views) {
    const uint32_t width = static_cast<uint32_t>(view.view.intrinsics.width);
    const uint32_t height = static_cast<uint32_t>(view.view.intrinsics.height);
    if (view.image.size() != static_cast<size_t>(width) * height * 3) {
      if (error != nullptr) *error = "a training image does not match its camera";
      return false;
    }
    max_pixels_ = std::max(max_pixels_, width * height);
    max_cols = std::max(max_cols, groupsFor(width, kTileSize));
    max_rows = std::max(max_rows, groupsFor(height, kTileSize));
  }
  tile_cols_ = max_cols;
  tile_rows_ = max_rows;
  if (tile_cols_ * tile_rows_ >= kMaxTiles) {
    if (error != nullptr) *error = "the training images are too large for the GPU path";
    return false;
  }

  pair_capacity_ = roundUp(capacity_ * kPairsPerSplatEstimate, kWorkgroup);
  host_.resize(capacity_, rest_stride_);
  host_.fromCloud(cloud, rest_stride_);

  if (!allocate(error)) return false;
  if (!buildPipelines(error)) return false;
  if (!uploadParameters()) return failWith(error, "could not upload the model to the GPU");

  if (!context_->beginCommands(error)) return false;
  context_->fill(statistics_, 0);
  context_->fill(visible_counts_, 0);
  context_->fill(moments_, 0);
  return context_->submitAndWait(error);
}

bool GpuTrainer::allocate(std::string* error) {
  const auto make = [&](Buffer* buffer, size_t bytes) {
    if (!error->empty()) return;
    *buffer = context_->createBuffer(static_cast<VkDeviceSize>(std::max<size_t>(bytes, 4)), true,
                                     error);
  };
  *error = std::string();
  const size_t n = capacity_;
  const size_t floats = sizeof(float);
  make(&params_, sizeof(ViewParamsBlock));
  make(&positions_, n * 3 * floats);
  make(&log_scales_, n * 3 * floats);
  make(&rotations_, n * 4 * floats);
  make(&opacities_, n * floats);
  make(&dc_, n * 3 * floats);
  make(&sh_rest_, n * rest_stride_ * floats);
  make(&grad_position_, n * 3 * floats);
  make(&grad_log_scale_, n * 3 * floats);
  make(&grad_rotation_, n * 4 * floats);
  make(&grad_opacity_, n * floats);
  make(&grad_dc_, n * 3 * floats);
  make(&grad_sh_rest_, n * rest_stride_ * floats);
  // One moment buffer for every group, interleaved (first, second) per value.
  const size_t moment_values = n * (3 + 3 + 4 + 1 + 3 + rest_stride_);
  make(&moments_, moment_values * 2 * floats);
  make(&statistics_, n * floats);
  make(&visible_counts_, n * sizeof(uint32_t));
  make(&proj_, n * 16 * floats);
  make(&meta_, n * sizeof(uint32_t));
  make(&tile_counts_, n * sizeof(uint32_t));
  make(&pair_offset_, n * sizeof(uint32_t));
  const size_t scan_blocks = groupsFor(capacity_, kScanBlock);
  make(&scan_sums_, scan_blocks * sizeof(uint32_t));
  make(&scan_scanned_, scan_blocks * sizeof(uint32_t));
  make(&scan_sums_top_, groupsFor(scan_blocks, kScanBlock) * sizeof(uint32_t));
  make(&screen_gradient_, n * 10 * sizeof(uint32_t));

  make(&pair_key_a_, pair_capacity_ * sizeof(uint32_t));
  make(&pair_key_b_, pair_capacity_ * sizeof(uint32_t));
  make(&pair_value_a_, pair_capacity_ * sizeof(uint32_t));
  make(&pair_value_b_, pair_capacity_ * sizeof(uint32_t));
  const size_t radix_blocks = groupsFor(pair_capacity_, kWorkgroup);
  const size_t histogram_size = kRadixDigits * radix_blocks;
  make(&histogram_, histogram_size * sizeof(uint32_t));
  make(&histogram_scanned_, histogram_size * sizeof(uint32_t));
  const size_t histogram_blocks = groupsFor(static_cast<uint32_t>(histogram_size), kScanBlock);
  make(&histogram_sums_, histogram_blocks * sizeof(uint32_t));
  make(&histogram_sums_scanned_, histogram_blocks * sizeof(uint32_t));
  make(&histogram_sums_top_,
       groupsFor(static_cast<uint32_t>(histogram_blocks), kScanBlock) * sizeof(uint32_t));
  if (histogram_blocks > kScanBlock || scan_blocks > kScanBlock) {
    if (error->empty()) *error = "the scene is too large for the GPU prefix sum";
    return false;
  }

  make(&tile_range_, static_cast<size_t>(kMaxTiles + 1) * 2 * sizeof(uint32_t));
  make(&rendered_, max_pixels_ * 3 * floats);
  make(&transmittance_, max_pixels_ * floats);
  make(&contributors_, max_pixels_ * sizeof(uint32_t));
  make(&dL_drgb_, max_pixels_ * 3 * floats);
  make(&target_, max_pixels_ * 3 * floats);
  make(&planes_, max_pixels_ * kSsimPlanes * floats);
  make(&scratch_, max_pixels_ * kSsimPlanes * floats);
  make(&blurred_, max_pixels_ * kSsimPlanes * floats);
  make(&terms_, max_pixels_ * kSsimPlanes * floats);
  make(&blurred_terms_, max_pixels_ * kSsimPlanes * floats);
  make(&totals_, 2 * sizeof(uint32_t));
  return error->empty();
}

bool GpuTrainer::buildPipelines(std::string* error) {
  const auto storage = [](uint32_t index, const Buffer& buffer) {
    return Binding{index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffer};
  };
  const auto uniform = [&](uint32_t index) {
    return Binding{index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &params_};
  };
  const auto build = [&](Pipeline* pipeline, const uint32_t* spirv, size_t bytes,
                         uint32_t push_size, const std::vector<Binding>& bindings) {
    if (!error->empty()) return;
    if (!context_->createPipeline(spirv, bytes / sizeof(uint32_t), push_size, bindings, pipeline,
                                  error)) {
      return;
    }
    owned_pipelines_.push_back(pipeline);
  };
  *error = std::string();

  build(&project_, shaders::kProject, sizeof(shaders::kProject), 0,
        {uniform(0), storage(1, positions_), storage(2, log_scales_), storage(3, rotations_),
         storage(4, opacities_), storage(5, dc_), storage(6, sh_rest_), storage(7, proj_),
         storage(8, meta_), storage(9, tile_counts_)});

  build(&scan_pairs_block_, shaders::kScanBlock, sizeof(shaders::kScanBlock), sizeof(ScanPush),
        {storage(0, tile_counts_), storage(1, pair_offset_), storage(2, scan_sums_)});
  build(&scan_pairs_top_, shaders::kScanBlock, sizeof(shaders::kScanBlock), sizeof(ScanPush),
        {storage(0, scan_sums_), storage(1, scan_scanned_), storage(2, scan_sums_top_)});
  build(&scan_pairs_add_, shaders::kScanAdd, sizeof(shaders::kScanAdd), sizeof(ScanPush),
        {storage(0, pair_offset_), storage(1, scan_scanned_)});

  build(&pair_write_, shaders::kPairWrite, sizeof(shaders::kPairWrite), 0,
        {uniform(0), storage(1, proj_), storage(2, meta_), storage(3, pair_offset_),
         storage(4, pair_key_a_), storage(5, pair_value_a_), storage(6, tile_counts_)});

  build(&radix_hist_a_, shaders::kRadixHistogram, sizeof(shaders::kRadixHistogram),
        sizeof(RadixPush), {storage(0, pair_key_a_), storage(1, histogram_)});
  build(&radix_hist_b_, shaders::kRadixHistogram, sizeof(shaders::kRadixHistogram),
        sizeof(RadixPush), {storage(0, pair_key_b_), storage(1, histogram_)});
  build(&scan_hist_block_, shaders::kScanBlock, sizeof(shaders::kScanBlock), sizeof(ScanPush),
        {storage(0, histogram_), storage(1, histogram_scanned_), storage(2, histogram_sums_)});
  build(&scan_hist_top_, shaders::kScanBlock, sizeof(shaders::kScanBlock), sizeof(ScanPush),
        {storage(0, histogram_sums_), storage(1, histogram_sums_scanned_),
         storage(2, histogram_sums_top_)});
  build(&scan_hist_add_, shaders::kScanAdd, sizeof(shaders::kScanAdd), sizeof(ScanPush),
        {storage(0, histogram_scanned_), storage(1, histogram_sums_scanned_)});
  build(&radix_scatter_ab_, shaders::kRadixScatter, sizeof(shaders::kRadixScatter),
        sizeof(RadixPush),
        {storage(0, pair_key_a_), storage(1, pair_value_a_), storage(2, histogram_scanned_),
         storage(3, pair_key_b_), storage(4, pair_value_b_)});
  build(&radix_scatter_ba_, shaders::kRadixScatter, sizeof(shaders::kRadixScatter),
        sizeof(RadixPush),
        {storage(0, pair_key_b_), storage(1, pair_value_b_), storage(2, histogram_scanned_),
         storage(3, pair_key_a_), storage(4, pair_value_a_)});

  build(&tile_ranges_, shaders::kTileRanges, sizeof(shaders::kTileRanges), 0,
        {uniform(0), storage(1, pair_key_a_), storage(2, tile_range_)});

  build(&forward_, shaders::kForward, sizeof(shaders::kForward), 0,
        {uniform(0), storage(1, proj_), storage(2, tile_range_), storage(3, pair_value_a_),
         storage(4, rendered_), storage(5, transmittance_), storage(6, contributors_)});

  build(&loss_l1_, shaders::kLossL1, sizeof(shaders::kLossL1), sizeof(LossPush),
        {storage(0, rendered_), storage(1, target_), storage(2, dL_drgb_), storage(3, totals_)});
  build(&ssim_prepare_, shaders::kSsimPrepare, sizeof(shaders::kSsimPrepare),
        sizeof(PreparePush),
        {storage(0, rendered_), storage(1, target_), storage(2, planes_)});
  build(&blur_planes_h_, shaders::kBlur, sizeof(shaders::kBlur), sizeof(BlurPush),
        {storage(0, planes_), storage(1, scratch_)});
  build(&blur_planes_v_, shaders::kBlur, sizeof(shaders::kBlur), sizeof(BlurPush),
        {storage(0, scratch_), storage(1, blurred_)});
  build(&ssim_terms_, shaders::kSsimTerms, sizeof(shaders::kSsimTerms), sizeof(SsimTermsPush),
        {storage(0, blurred_), storage(1, terms_), storage(2, totals_)});
  build(&blur_terms_h_, shaders::kBlur, sizeof(shaders::kBlur), sizeof(BlurPush),
        {storage(0, terms_), storage(1, scratch_)});
  build(&blur_terms_v_, shaders::kBlur, sizeof(shaders::kBlur), sizeof(BlurPush),
        {storage(0, scratch_), storage(1, blurred_terms_)});
  build(&ssim_gradient_, shaders::kSsimGradient, sizeof(shaders::kSsimGradient),
        sizeof(SsimGradientPush),
        {storage(0, planes_), storage(1, blurred_terms_), storage(2, dL_drgb_)});

  build(&backward_, shaders::kBackward, sizeof(shaders::kBackward), 0,
        {uniform(0), storage(1, proj_), storage(2, tile_range_), storage(3, pair_value_a_),
         storage(4, transmittance_), storage(5, contributors_), storage(6, dL_drgb_),
         storage(7, screen_gradient_)});

  build(&backward_params_, shaders::kBackwardParams, sizeof(shaders::kBackwardParams), 0,
        {uniform(0), storage(1, positions_), storage(2, log_scales_), storage(3, rotations_),
         storage(4, sh_rest_), storage(5, proj_), storage(6, meta_), storage(7, screen_gradient_),
         storage(8, grad_position_), storage(9, grad_log_scale_), storage(10, grad_rotation_),
         storage(11, grad_opacity_), storage(12, grad_dc_), storage(13, grad_sh_rest_),
         storage(14, statistics_), storage(15, visible_counts_)});

  build(&adam_, shaders::kAdam, sizeof(shaders::kAdam), sizeof(AdamPush),
        {storage(0, positions_), storage(1, log_scales_), storage(2, rotations_),
         storage(3, opacities_), storage(4, dc_), storage(5, sh_rest_),
         storage(6, grad_position_), storage(7, grad_log_scale_), storage(8, grad_rotation_),
         storage(9, grad_opacity_), storage(10, grad_dc_), storage(11, grad_sh_rest_),
         storage(12, moments_)});
  return error->empty();
}

void GpuTrainer::recordScan(const Pipeline& block, const Pipeline& top, const Pipeline& add,
                            uint32_t count, uint32_t level1_count) {
  ScanPush push{count};
  context_->dispatch(block, groupsFor(count, kScanBlock), 1, &push, sizeof(push));
  context_->barrier();
  ScanPush top_push{level1_count};
  context_->dispatch(top, groupsFor(level1_count, kScanBlock), 1, &top_push, sizeof(top_push));
  context_->barrier();
  context_->dispatch(add, groupsFor(count, kWorkgroup), 1, &push, sizeof(push));
  context_->barrier();
}

void GpuTrainer::recordIteration(const SplatTrainingView& view) {
  const uint32_t width = static_cast<uint32_t>(view.view.intrinsics.width);
  const uint32_t height = static_cast<uint32_t>(view.view.intrinsics.height);
  const uint32_t pixels = width * height;
  const uint32_t values = pixels * 3;
  const uint32_t cols = groupsFor(width, kTileSize);
  const uint32_t rows = groupsFor(height, kTileSize);

  context_->fill(grad_position_, 0);
  context_->fill(grad_log_scale_, 0);
  context_->fill(grad_rotation_, 0);
  context_->fill(grad_opacity_, 0);
  context_->fill(grad_dc_, 0);
  context_->fill(grad_sh_rest_, 0);
  context_->fill(screen_gradient_, 0);
  context_->fill(tile_range_, 0);
  context_->fill(totals_, 0);
  // Every pair slot the splats do not fill has to look like a key that sorts
  // past the end, or the leftovers from the last iteration would be rendered.
  context_->fill(pair_key_a_, 0xFFFFFFFFu);
  context_->barrier();

  context_->dispatch(project_, groupsFor(active_, kWorkgroup));
  context_->barrier();

  recordScan(scan_pairs_block_, scan_pairs_top_, scan_pairs_add_, active_,
             groupsFor(active_, kScanBlock));
  context_->dispatch(pair_write_, groupsFor(active_, kWorkgroup));
  context_->barrier();

  const uint32_t radix_blocks = groupsFor(pair_capacity_, kWorkgroup);
  const uint32_t histogram_size = kRadixDigits * radix_blocks;
  for (uint32_t pass = 0; pass < kRadixPasses; ++pass) {
    const bool from_a = (pass % 2) == 0;
    RadixPush push{pair_capacity_, pass * 4u, radix_blocks};
    context_->fill(histogram_, 0);
    context_->barrier();
    context_->dispatch(from_a ? radix_hist_a_ : radix_hist_b_, radix_blocks, 1, &push,
                       sizeof(push));
    context_->barrier();
    recordScan(scan_hist_block_, scan_hist_top_, scan_hist_add_, histogram_size,
               groupsFor(histogram_size, kScanBlock));
    context_->dispatch(from_a ? radix_scatter_ab_ : radix_scatter_ba_, radix_blocks, 1, &push,
                       sizeof(push));
    context_->barrier();
  }

  context_->dispatch(tile_ranges_, groupsFor(pair_capacity_, kWorkgroup));
  context_->barrier();

  context_->dispatch(forward_, cols, rows);
  context_->barrier();

  LossPush loss{values, config_.ssim_weight, 0};
  context_->dispatch(loss_l1_, groupsFor(values, kWorkgroup), 1, &loss, sizeof(loss));
  context_->barrier();

  if (config_.ssim_weight > 0.f) {
    for (uint32_t channel = 0; channel < 3; ++channel) {
      PreparePush prepare{pixels, channel};
      context_->dispatch(ssim_prepare_, groupsFor(pixels, kWorkgroup), 1, &prepare,
                         sizeof(prepare));
      context_->barrier();
      BlurPush horizontal{width, height, kSsimPlanes, 0};
      BlurPush vertical{width, height, kSsimPlanes, 1};
      context_->dispatch(blur_planes_h_, groupsFor(width, 64), groupsFor(height, 4), &horizontal,
                         sizeof(horizontal));
      context_->barrier();
      context_->dispatch(blur_planes_v_, groupsFor(width, 64), groupsFor(height, 4), &vertical,
                         sizeof(vertical));
      context_->barrier();
      SsimTermsPush terms{pixels, 1.f / static_cast<float>(values), 1, 1};
      context_->dispatch(ssim_terms_, groupsFor(pixels, kWorkgroup), 1, &terms, sizeof(terms));
      context_->barrier();
      context_->dispatch(blur_terms_h_, groupsFor(width, 64), groupsFor(height, 4), &horizontal,
                         sizeof(horizontal));
      context_->barrier();
      context_->dispatch(blur_terms_v_, groupsFor(width, 64), groupsFor(height, 4), &vertical,
                         sizeof(vertical));
      context_->barrier();
      SsimGradientPush gradient{pixels, channel, config_.ssim_weight};
      context_->dispatch(ssim_gradient_, groupsFor(pixels, kWorkgroup), 1, &gradient,
                         sizeof(gradient));
      context_->barrier();
    }
  }

  context_->dispatch(backward_, cols, rows);
  context_->barrier();
  context_->dispatch(backward_params_, groupsFor(active_, kWorkgroup));
  context_->barrier();
}

float GpuTrainer::positionLearningRate(int iteration) const {
  const float t = config_.iterations <= 1
                      ? 1.f
                      : static_cast<float>(iteration) / static_cast<float>(config_.iterations - 1);
  const float ratio = config_.position_lr_final / std::max(config_.position_lr_initial, 1e-30f);
  return config_.position_lr_initial * std::pow(ratio, std::clamp(t, 0.f, 1.f)) * extent_;
}

bool GpuTrainer::uploadParameters() {
  const size_t n = capacity_;
  return context_->upload(positions_, host_.position.data(), n * 3 * sizeof(float)) &&
         context_->upload(log_scales_, host_.log_scale.data(), n * 3 * sizeof(float)) &&
         context_->upload(rotations_, host_.rotation.data(), n * 4 * sizeof(float)) &&
         context_->upload(opacities_, host_.opacity.data(), n * sizeof(float)) &&
         context_->upload(dc_, host_.dc.data(), n * 3 * sizeof(float)) &&
         context_->upload(sh_rest_, host_.sh_rest.data(), n * rest_stride_ * sizeof(float));
}

bool GpuTrainer::downloadParameters() {
  const size_t n = capacity_;
  return context_->download(positions_, host_.position.data(), n * 3 * sizeof(float)) &&
         context_->download(log_scales_, host_.log_scale.data(), n * 3 * sizeof(float)) &&
         context_->download(rotations_, host_.rotation.data(), n * 4 * sizeof(float)) &&
         context_->download(opacities_, host_.opacity.data(), n * sizeof(float)) &&
         context_->download(dc_, host_.dc.data(), n * 3 * sizeof(float)) &&
         context_->download(sh_rest_, host_.sh_rest.data(), n * rest_stride_ * sizeof(float));
}

bool GpuTrainer::densify(GaussianCloud* cloud, int iteration, std::string* error) {
  if (!downloadParameters()) return failWith(error, "could not read the model back from the GPU");
  cloud->gaussians.resize(active_);
  host_.toCloud(cloud, rest_stride_);

  SplatDensifyStats statistics;
  statistics.screen_gradient.resize(active_);
  statistics.visible_count.resize(active_);
  host_statistics_.resize(capacity_);
  host_visible_counts_.resize(capacity_);
  context_->download(statistics_, host_statistics_.data(), capacity_ * sizeof(float));
  context_->download(visible_counts_, host_visible_counts_.data(), capacity_ * sizeof(uint32_t));
  for (uint32_t g = 0; g < active_; ++g) {
    statistics.screen_gradient[g] = host_statistics_[g];
    statistics.visible_count[g] = static_cast<int>(host_visible_counts_[g]);
  }

  SplatTrainingConfig bounded = config_;
  bounded.max_gaussians = std::min(config_.max_gaussians, static_cast<int>(capacity_));
  const SplatDensifyResult result =
      densifyAndPrune(cloud, statistics, bounded, extent_, iteration, &densify_rng_);

  // The optimiser moments move with their gaussians; new ones start at zero.
  const size_t per_gaussian = 3 + 3 + 4 + 1 + 3 + rest_stride_;
  host_moments_.resize(static_cast<size_t>(capacity_) * per_gaussian * 2);
  context_->download(moments_, host_moments_.data(), host_moments_.size() * sizeof(float));
  std::vector<float> next(host_moments_.size(), 0.f);
  const size_t group_sizes[6] = {3, 3, 4, 1, 3, rest_stride_};
  size_t base = 0;
  for (size_t group = 0; group < 6; ++group) {
    const size_t stride = group_sizes[group];
    for (size_t i = 0; i < result.source_index.size(); ++i) {
      if (result.source_index[i] < 0) continue;
      const size_t from = (base + static_cast<size_t>(result.source_index[i]) * stride) * 2;
      const size_t to = (base + i * stride) * 2;
      std::memcpy(&next[to], &host_moments_[from], stride * 2 * sizeof(float));
    }
    base += static_cast<size_t>(capacity_) * stride;
  }
  host_moments_.swap(next);
  context_->upload(moments_, host_moments_.data(), host_moments_.size() * sizeof(float));

  active_ = static_cast<uint32_t>(cloud->gaussians.size());
  host_.resize(capacity_, rest_stride_);
  host_.fromCloud(*cloud, rest_stride_);
  if (!uploadParameters()) return failWith(error, "could not upload the model to the GPU");

  // Check the pair array is still big enough for how the scene has grown.
  host_tile_counts_.resize(capacity_);
  context_->download(tile_counts_, host_tile_counts_.data(), capacity_ * sizeof(uint32_t));
  uint64_t used = 0;
  for (uint32_t g = 0; g < active_ && g < capacity_; ++g) used += host_tile_counts_[g];
  if (used * 5 > static_cast<uint64_t>(pair_capacity_) * 4) {
    return growPairCapacity(static_cast<uint32_t>(std::min<uint64_t>(used * 2, 1u << 26)), error);
  }
  return true;
}

bool GpuTrainer::growPairCapacity(uint32_t wanted, std::string* error) {
  // Rebuilding the pair buffers means rebuilding the descriptor sets that
  // point at them, which is simplest done by rebuilding every pipeline.
  const uint32_t grown = roundUp(wanted, kWorkgroup);
  if (grown <= pair_capacity_) return true;
  const uint32_t radix_blocks = groupsFor(grown, kWorkgroup);
  if (groupsFor(kRadixDigits * radix_blocks, kScanBlock) > kScanBlock) {
    // Staying at the current size only risks clipping the largest splats,
    // which is a visual compromise rather than a failure.
    return true;
  }
  // allocate() makes every buffer afresh, so the moments have to be carried
  // across by hand or training would restart from no momentum at all.
  const size_t per_gaussian = 3 + 3 + 4 + 1 + 3 + rest_stride_;
  host_moments_.resize(static_cast<size_t>(capacity_) * per_gaussian * 2);
  context_->download(moments_, host_moments_.data(), host_moments_.size() * sizeof(float));

  for (Pipeline* pipeline : owned_pipelines_) context_->destroyPipeline(pipeline);
  owned_pipelines_.clear();
  pair_capacity_ = grown;
  std::string scratch;
  if (!allocate(&scratch)) {
    if (error != nullptr) *error = scratch;
    return false;
  }
  if (!buildPipelines(&scratch)) {
    if (error != nullptr) *error = scratch;
    return false;
  }
  context_->upload(moments_, host_moments_.data(), host_moments_.size() * sizeof(float));
  return uploadParameters();
}

bool GpuTrainer::run(GaussianCloud* cloud, const std::vector<SplatTrainingView>& views,
                     const SplatTrainingCallback& callback, bool* cancelled,
                     std::string* error) {
  *cancelled = false;
  std::vector<int> order(views.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  uint32_t shuffle = config_.seed == 0u ? 1u : config_.seed;
  const auto nextRandom = [&]() {
    shuffle ^= shuffle << 13;
    shuffle ^= shuffle >> 17;
    shuffle ^= shuffle << 5;
    return shuffle;
  };

  float smoothed = 0.f;
  size_t cursor = order.size();
  for (int iteration = 0; iteration < config_.iterations; ++iteration) {
    if (cursor >= order.size()) {
      for (size_t i = order.size(); i > 1; --i) {
        std::swap(order[i - 1], order[nextRandom() % i]);
      }
      cursor = 0;
    }
    const SplatTrainingView& view = views[static_cast<size_t>(order[cursor++])];

    const int sh_degree =
        config_.sh_increase_interval > 0
            ? std::min(config_.sh_degree, iteration / config_.sh_increase_interval)
            : config_.sh_degree;

    ViewParamsBlock block{};
    const Eigen::Matrix3f R = view.view.camera_from_world.R.cast<float>();
    const Vec3f t = view.view.camera_from_world.t.cast<float>();
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 3; ++row) block.camera_from_world[col * 4 + row] = R(row, col);
    }
    for (int row = 0; row < 3; ++row) block.camera_from_world[12 + row] = t[row];
    block.camera_from_world[15] = 1.f;
    const Vec3f centre = view.view.camera_from_world.center().cast<float>();
    for (int a = 0; a < 3; ++a) block.camera_center[a] = centre[a];
    block.intrinsics[0] = static_cast<float>(view.view.intrinsics.fx);
    block.intrinsics[1] = static_cast<float>(view.view.intrinsics.fy);
    block.intrinsics[2] = static_cast<float>(view.view.intrinsics.cx);
    block.intrinsics[3] = static_cast<float>(view.view.intrinsics.cy);
    block.size[0] = view.view.intrinsics.width;
    block.size[1] = view.view.intrinsics.height;
    block.size[2] = static_cast<int32_t>(active_);
    block.size[3] = sh_degree;
    const RasterOptions defaults;
    block.options[0] = defaults.screen_dilation;
    block.options[1] = defaults.min_alpha;
    block.options[2] = defaults.transmittance_cutoff;
    block.options[3] = config_.ssim_weight;
    block.tiles[0] = static_cast<int32_t>(groupsFor(
        static_cast<uint32_t>(view.view.intrinsics.width), kTileSize));
    block.tiles[1] = static_cast<int32_t>(groupsFor(
        static_cast<uint32_t>(view.view.intrinsics.height), kTileSize));
    block.tiles[2] = static_cast<int32_t>(rest_stride_);
    block.tiles[3] = static_cast<int32_t>(pair_capacity_);
    // The sort key quantises depth over this range; the camera extent is a
    // generous bound on how far a gaussian can sensibly be.
    block.depth_range[0] = 0.01f;
    block.depth_range[1] = std::max(extent_ * 4.f, 1.f);

    context_->upload(params_, &block, sizeof(block));
    context_->upload(target_, view.image.data(), view.image.size() * sizeof(float));

    if (!context_->beginCommands(error)) return false;
    recordIteration(view);

    AdamPush adam{};
    adam.lr_position = positionLearningRate(iteration);
    adam.lr_scale = config_.scale_lr;
    adam.lr_rotation = config_.rotation_lr;
    adam.lr_opacity = config_.opacity_lr;
    adam.lr_dc = config_.dc_lr;
    adam.lr_sh_rest = config_.dc_lr / std::max(1.f, config_.sh_rest_lr_divider);
    adam.beta1 = config_.adam_beta1;
    adam.beta2 = config_.adam_beta2;
    adam.epsilon = config_.adam_epsilon;
    adam.correction1 = 1.f - std::pow(config_.adam_beta1, static_cast<float>(iteration + 1));
    adam.correction2 = 1.f - std::pow(config_.adam_beta2, static_cast<float>(iteration + 1));
    adam.count = active_;
    adam.rest_stride = rest_stride_;
    uint32_t base = 0;
    adam.moment_base_position = base;
    base += capacity_ * 3;
    adam.moment_base_log_scale = base;
    base += capacity_ * 3;
    adam.moment_base_rotation = base;
    base += capacity_ * 4;
    adam.moment_base_opacity = base;
    base += capacity_;
    adam.moment_base_dc = base;
    base += capacity_ * 3;
    adam.moment_base_sh_rest = base;
    context_->dispatch(adam_, groupsFor(active_, kWorkgroup), 1, &adam, sizeof(adam));

    if (!context_->submitAndWait(error)) return false;

    float totals[2] = {0.f, 0.f};
    context_->download(totals_, totals, sizeof(totals));
    const float loss = (1.f - config_.ssim_weight) * totals[0] +
                       config_.ssim_weight * (1.f - totals[1]);
    smoothed = iteration == 0 ? loss : 0.99f * smoothed + 0.01f * loss;

    const int next_iteration = iteration + 1;
    if (next_iteration >= config_.densify_from_iteration &&
        next_iteration <= config_.densify_until_iteration &&
        next_iteration % config_.densify_interval == 0) {
      if (!densify(cloud, next_iteration, error)) return false;
      if (!context_->beginCommands(error)) return false;
      context_->fill(statistics_, 0);
      context_->fill(visible_counts_, 0);
      if (!context_->submitAndWait(error)) return false;
    }

    if (callback != nullptr) {
      SplatTrainingProgress progress;
      progress.iteration = next_iteration;
      progress.total_iterations = config_.iterations;
      progress.gaussian_count = static_cast<int>(active_);
      progress.loss = loss;
      progress.smoothed_loss = smoothed;
      if (!callback(progress)) {
        *cancelled = true;
        break;
      }
    }
  }

  if (!downloadParameters()) return failWith(error, "could not read the model back from the GPU");
  cloud->gaussians.resize(active_);
  host_.toCloud(cloud, rest_stride_);
  cloud->sh_degree = config_.sh_degree;
  return true;
}

}  // namespace

SplatGpuAvailability splatGpuAvailability() {
  SplatGpuAvailability availability;
  std::string error;
  std::unique_ptr<Context> context = Context::create(&error);
  if (context == nullptr) {
    availability.reason = error;
    return availability;
  }
  availability.available = true;
  availability.device_name = context->deviceName();
  return availability;
}

bool trainSplatsOnGpu(GaussianCloud* cloud, const std::vector<SplatTrainingView>& views,
                      const SplatTrainingConfig& config, const SplatTrainingCallback& callback,
                      std::string* error) {
  std::string local;
  std::unique_ptr<Context> context = Context::create(&local);
  if (context == nullptr) {
    if (error != nullptr) *error = local;
    return false;
  }
  GpuTrainer trainer(std::move(context), config);
  if (!trainer.setUp(*cloud, views, &local)) {
    if (error != nullptr) *error = local;
    return false;
  }
  bool cancelled = false;
  if (!trainer.run(cloud, views, callback, &cancelled, &local)) {
    if (error != nullptr) *error = local;
    return false;
  }
  cloud->prune(config.prune_opacity);
  cloud->sortByOpacity();
  return true;
}

}  // namespace parallax

#endif
