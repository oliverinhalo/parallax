// Differentiable rasterisation of 3D Gaussians (EWA splatting).
//
// This is the mathematics of Gaussian splatting, written so that it can be run
// and checked on a host machine. The Vulkan compute shaders in
// `src/splat/vulkan/shaders/` implement the same equations for the device; the
// tests here are what establish that those equations are right, because the
// shaders themselves cannot be run in CI (see docs/DECISIONS.md, ADR-009).
//
// Conventions follow Kerbl et al. 2023 so that clouds trained here load in
// other viewers without a conversion step:
//   * covariance   Sigma = R S S^T R^T, with S = diag(exp(log_scale))
//   * projection   the affine (Jacobian) approximation of the perspective map
//   * compositing  front-to-back, alpha = sigmoid(opacity) * exp(power)
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/splat/GaussianCloud.h"

#include <array>
#include <vector>

namespace parallax {

/// A camera to render from: intrinsics plus a camera-from-world pose.
struct SplatView {
  CameraIntrinsics intrinsics;
  Pose camera_from_world;
};

struct RasterOptions {
  std::array<float, 3> background{0.f, 0.f, 0.f};
  /// Spherical-harmonic bands to evaluate. Lower than the cloud's own degree
  /// during the warm-up, so the model fits flat colour before view dependence.
  int sh_degree = 0;
  /// Below this the splat cannot change a pixel by one code value, so it is
  /// skipped. Saves roughly half the per-pixel work on a trained cloud.
  float min_alpha = 1.f / 255.f;
  /// Stop walking a pixel's list once this little light is left to block.
  float transmittance_cutoff = 1e-4f;
  /**
   * Isotropic dilation added to the 2D covariance, in pixels squared.
   *
   * Without it a gaussian that projects to less than a pixel falls between the
   * sample points and flickers as the camera moves. 0.3 is the reference
   * implementation's value and corresponds to a little over half a pixel of
   * standard deviation.
   */
  float screen_dilation = 0.3f;
};

/// A gaussian after projection: everything the pixel loop and the backward
/// pass need, so neither recomputes the camera transform.
struct ProjectedSplat {
  int index = -1;
  Vec2f mean = Vec2f::Zero();
  /// 2D covariance as (xx, xy, yy).
  Vec3f cov2d = Vec3f::Zero();
  /// Its inverse, same packing. The pixel loop uses this directly.
  Vec3f conic = Vec3f::Zero();
  float depth = 0.f;
  /// Three standard deviations of the larger axis, in pixels.
  float radius = 0.f;
  std::array<float, 3> color{0.f, 0.f, 0.f};
  float opacity = 0.f;
  /// Set where the spherical-harmonic sum went negative and was clamped to
  /// zero. The clamp is not differentiable, so the backward pass drops the
  /// gradient for that channel rather than pushing against a wall.
  std::array<bool, 3> color_clamped{false, false, false};
};

struct SplatImage {
  int width = 0;
  int height = 0;
  /// Row-major RGB, three floats per pixel, linear and unbounded above.
  std::vector<float> rgb;
  /// Light still unblocked after the last contributing gaussian, per pixel.
  /// The backward pass needs it, and it doubles as an alpha channel.
  std::vector<float> transmittance;

  float at(int x, int y, int channel) const {
    return rgb[(static_cast<size_t>(y) * width + x) * 3 + channel];
  }
};

/// Accumulated gradients, one entry per gaussian in the cloud.
struct SplatGradients {
  std::vector<Vec3f> position;
  std::vector<Vec3f> log_scale;
  /// Quaternion gradient packed (w, x, y, z) -- not Eigen's internal
  /// (x, y, z, w) coefficient order. Taken with respect to the quaternion as
  /// stored, including the normalisation the rasteriser applies to it.
  std::vector<Eigen::Vector4f> rotation;
  std::vector<float> opacity_logit;
  std::vector<std::array<float, 3>> dc;
  /// Flattened like `Gaussian::sh_rest`: `sh_rest_stride` floats per gaussian,
  /// which is three times the per-channel coefficient count.
  std::vector<float> sh_rest;
  int sh_rest_stride = 0;

  /**
   * Magnitude of the loss gradient in screen space, summed over the views a
   * gaussian appeared in, with `visible_count` the number of those views.
   *
   * This is the densification signal: a gaussian that the loss keeps trying to
   * drag across the image is one gaussian standing in for detail that needs
   * two.
   */
  std::vector<float> screen_gradient;
  std::vector<int> visible_count;

  void resize(size_t count, int rest_stride);
  void clear();
};

/// Evaluate the spherical-harmonic colour for one gaussian along `direction`
/// (unit, pointing from the camera towards the gaussian). Clamped at zero.
std::array<float, 3> evaluateSh(const Gaussian& gaussian, int degree, const Vec3f& direction,
                                std::array<bool, 3>* clamped = nullptr);

/// Project every gaussian; the result holds only those in front of the camera
/// and overlapping the image, sorted near to far.
std::vector<ProjectedSplat> projectSplats(const GaussianCloud& cloud, const SplatView& view,
                                          const RasterOptions& options);

SplatImage rasterizeSplats(const std::vector<ProjectedSplat>& projected, const SplatView& view,
                           const RasterOptions& options);

/// Convenience: project then rasterise.
SplatImage renderSplats(const GaussianCloud& cloud, const SplatView& view,
                        const RasterOptions& options);

/**
 * Backward pass. `dL_drgb` is the loss gradient with respect to every rendered
 * channel, laid out like `SplatImage::rgb`; gradients are *added* to `out`, so
 * a caller can accumulate over several views before stepping.
 */
void rasterizeSplatsBackward(const GaussianCloud& cloud, const SplatView& view,
                             const RasterOptions& options,
                             const std::vector<ProjectedSplat>& projected,
                             const SplatImage& forward, const std::vector<float>& dL_drgb,
                             SplatGradients* out);

// ------------------------------------------------------------------ loss ---

/**
 * The training loss: `(1 - ssim_weight) * L1 + ssim_weight * (1 - SSIM)`.
 *
 * L1 alone converges to a blurry mean; SSIM is what makes the result look
 * sharp, because it penalises losing local structure rather than losing
 * brightness. Both terms and the gradient are computed in one pass.
 */
float photometricLoss(const SplatImage& rendered, const std::vector<float>& target,
                      float ssim_weight, std::vector<float>* dL_drgb);

/// Mean structural similarity between two equally sized RGB images, and
/// optionally its gradient with respect to `rendered`.
float meanSsim(const std::vector<float>& rendered, const std::vector<float>& target, int width,
               int height, std::vector<float>* d_rendered = nullptr);

}  // namespace parallax
