#include "parallax/splat/SplatRaster.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>

#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

SplatView makeView(int width = 48, int height = 36, double distance = 2.0) {
  SplatView view;
  view.intrinsics.width = width;
  view.intrinsics.height = height;
  view.intrinsics.fx = view.intrinsics.fy = 0.9 * width;
  view.intrinsics.cx = 0.5 * width;
  view.intrinsics.cy = 0.5 * height;
  // Looking down +Z from -Z, i.e. the camera sits at (0, 0, -distance).
  view.camera_from_world = Pose(Mat3::Identity(), Vec3(0.0, 0.0, distance));
  return view;
}

Gaussian makeGaussian(const Vec3f& position, float log_scale, float opacity_logit,
                      int rest_per_channel, Rng* rng) {
  Gaussian g;
  g.position = position;
  g.log_scale = Vec3f(log_scale, log_scale - 0.2f, log_scale + 0.3f);
  Eigen::Quaternionf q(static_cast<float>(rng->normal()), static_cast<float>(rng->normal()),
                       static_cast<float>(rng->normal()), static_cast<float>(rng->normal()));
  if (q.norm() < 1e-3f) q = Eigen::Quaternionf::Identity();
  g.rotation = q.normalized();
  g.opacity_logit = opacity_logit;
  for (int c = 0; c < 3; ++c) g.dc[c] = static_cast<float>(rng->uniform(0.2, 1.2));
  g.sh_rest.assign(static_cast<size_t>(rest_per_channel) * 3, 0.f);
  for (float& value : g.sh_rest) value = static_cast<float>(rng->uniform(-0.3, 0.3));
  return g;
}

/// A handful of overlapping gaussians in front of the camera, large enough to
/// cover many pixels so every term in the backward pass is exercised.
GaussianCloud makeOverlappingCloud(int sh_degree, size_t count = 6) {
  GaussianCloud cloud;
  cloud.sh_degree = sh_degree;
  const int rest = shRestPerChannel(sh_degree);
  Rng rng(77);
  for (size_t i = 0; i < count; ++i) {
    const Vec3f position(static_cast<float>(rng.uniform(-0.4, 0.4)),
                         static_cast<float>(rng.uniform(-0.3, 0.3)),
                         static_cast<float>(rng.uniform(-0.3, 0.3)));
    cloud.gaussians.push_back(
        makeGaussian(position, static_cast<float>(rng.uniform(-2.6, -2.0)),
                     static_cast<float>(rng.uniform(-0.5, 1.5)), rest, &rng));
  }
  return cloud;
}

std::vector<float> makeTarget(size_t values, unsigned seed) {
  Rng rng(seed);
  std::vector<float> target(values);
  for (float& value : target) value = static_cast<float>(rng.uniform(0.0, 1.0));
  return target;
}

struct LossAndGradients {
  double loss = 0.0;
  SplatGradients gradients;
};

/// Render the cloud, score it against a fixed target and back-propagate.
LossAndGradients evaluate(const GaussianCloud& cloud, const SplatView& view,
                          const RasterOptions& options, const std::vector<float>& target,
                          float ssim_weight, bool want_gradients) {
  LossAndGradients result;
  const std::vector<ProjectedSplat> projected = projectSplats(cloud, view, options);
  const SplatImage image = rasterizeSplats(projected, view, options);
  std::vector<float> dL_drgb;
  result.loss = photometricLoss(image, target, ssim_weight, want_gradients ? &dL_drgb : nullptr);
  if (!want_gradients) return result;
  result.gradients.resize(cloud.size(), static_cast<int>(cloud.gaussians.front().sh_rest.size()));
  rasterizeSplatsBackward(cloud, view, options, projected, image, dL_drgb, &result.gradients);
  return result;
}

/**
 * Score the render with a fixed linear functional of its pixels instead of a
 * photometric loss.
 *
 * A gradient check needs a smooth objective. L1 is not: its derivative jumps
 * wherever a pixel crosses the target, so a finite difference straddling that
 * point disagrees with the analytic gradient for reasons that have nothing to
 * do with the rasteriser. Weighting the pixels by a fixed random vector gives
 * an objective whose gradient with respect to the image is exactly those
 * weights, which isolates the backward pass being tested.
 */
LossAndGradients evaluateLinear(const GaussianCloud& cloud, const SplatView& view,
                                const RasterOptions& options, const std::vector<float>& weights,
                                bool want_gradients) {
  LossAndGradients result;
  const std::vector<ProjectedSplat> projected = projectSplats(cloud, view, options);
  const SplatImage image = rasterizeSplats(projected, view, options);
  for (size_t i = 0; i < image.rgb.size(); ++i) {
    result.loss += static_cast<double>(weights[i]) * image.rgb[i];
  }
  if (!want_gradients) return result;
  result.gradients.resize(cloud.size(), static_cast<int>(cloud.gaussians.front().sh_rest.size()));
  rasterizeSplatsBackward(cloud, view, options, projected, image, weights, &result.gradients);
  return result;
}

/// Central-difference check of one scalar parameter.
double numericGradient(const std::function<void(float)>& set, float value, double step,
                       const std::function<double()>& loss) {
  set(static_cast<float>(value + step));
  const double plus = loss();
  set(static_cast<float>(value - step));
  const double minus = loss();
  set(value);
  return (plus - minus) / (2.0 * step);
}

/// Relative agreement between an analytic and a numeric gradient, scaled so
/// that two near-zero values count as agreeing.
double relativeError(double analytic, double numeric) {
  const double scale = std::max({std::abs(analytic), std::abs(numeric), 1e-3});
  return std::abs(analytic - numeric) / scale;
}

}  // namespace

TEST(SplatRaster, DegreeZeroColourMatchesTheDcConversion) {
  Rng rng(1);
  const Gaussian g = makeGaussian(Vec3f::Zero(), -2.f, 0.f, 0, &rng);
  const std::array<float, 3> expected = gaussianBaseColor(g);
  const std::array<float, 3> actual = evaluateSh(g, 0, Vec3f::UnitZ());
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(expected[c], actual[c], 1e-6f);
}

TEST(SplatRaster, SphericalHarmonicsVaryWithTheViewDirection) {
  Rng rng(2);
  const Gaussian g = makeGaussian(Vec3f::Zero(), -2.f, 0.f, shRestPerChannel(3), &rng);
  const std::array<float, 3> front = evaluateSh(g, 3, Vec3f::UnitZ());
  const std::array<float, 3> side = evaluateSh(g, 3, Vec3f::UnitX());
  bool differs = false;
  for (int c = 0; c < 3; ++c) differs = differs || std::abs(front[c] - side[c]) > 1e-3f;
  EXPECT_TRUE(differs);

  // Degree 0 ignores direction entirely, which is what the warm-up relies on.
  const std::array<float, 3> dc_front = evaluateSh(g, 0, Vec3f::UnitZ());
  const std::array<float, 3> dc_side = evaluateSh(g, 0, Vec3f::UnitX());
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(dc_front[c], dc_side[c], 1e-7f);
}

TEST(SplatRaster, NegativeHarmonicSumsAreClampedAndReported) {
  Rng rng(3);
  Gaussian g = makeGaussian(Vec3f::Zero(), -2.f, 0.f, 0, &rng);
  g.dc = {-10.f, -10.f, 2.f};
  std::array<bool, 3> clamped{};
  const std::array<float, 3> color = evaluateSh(g, 0, Vec3f::UnitZ(), &clamped);
  EXPECT_FLOAT_EQ(0.f, color[0]);
  EXPECT_FLOAT_EQ(0.f, color[1]);
  EXPECT_GT(color[2], 0.f);
  EXPECT_TRUE(clamped[0]);
  EXPECT_TRUE(clamped[1]);
  EXPECT_FALSE(clamped[2]);
}

TEST(SplatRaster, AGaussianOnTheOpticalAxisProjectsToThePrincipalPoint) {
  GaussianCloud cloud;
  Rng rng(4);
  cloud.gaussians.push_back(makeGaussian(Vec3f::Zero(), -2.f, 2.f, 0, &rng));
  const SplatView view = makeView();
  const std::vector<ProjectedSplat> projected = projectSplats(cloud, view, {});
  ASSERT_EQ(1u, projected.size());
  EXPECT_NEAR(view.intrinsics.cx, projected[0].mean.x(), 1e-4);
  EXPECT_NEAR(view.intrinsics.cy, projected[0].mean.y(), 1e-4);
  EXPECT_NEAR(2.0, projected[0].depth, 1e-5);
}

TEST(SplatRaster, AnIsotropicGaussianProjectsToACircleThatShrinksWithDistance) {
  GaussianCloud cloud;
  Gaussian g;
  g.position = Vec3f::Zero();
  g.log_scale = Vec3f::Constant(std::log(0.05f));
  g.opacity_logit = 3.f;
  cloud.gaussians.push_back(g);

  // The screen-space dilation is a fixed number of pixels, so it would stop
  // the radius following 1/z exactly; this test is about the projection.
  RasterOptions options;
  options.screen_dilation = 0.f;
  const auto radiusAt = [&](double distance) {
    const std::vector<ProjectedSplat> projected =
        projectSplats(cloud, makeView(48, 36, distance), options);
    EXPECT_EQ(1u, projected.size());
    // An isotropic 3D gaussian seen head on is isotropic in 2D too.
    EXPECT_NEAR(projected[0].cov2d.x(), projected[0].cov2d.z(), 1e-3f);
    EXPECT_NEAR(0.f, projected[0].cov2d.y(), 1e-3f);
    return projected[0].radius;
  };

  const float near_radius = radiusAt(1.0);
  const float far_radius = radiusAt(2.0);
  // Screen size goes as 1/z, so doubling the distance halves it.
  EXPECT_NEAR(0.5f, far_radius / near_radius, 0.01f);
}

TEST(SplatRaster, GaussiansBehindTheCameraAreDropped) {
  GaussianCloud cloud;
  Rng rng(5);
  cloud.gaussians.push_back(makeGaussian(Vec3f(0.f, 0.f, 0.f), -2.f, 3.f, 0, &rng));
  cloud.gaussians.push_back(makeGaussian(Vec3f(0.f, 0.f, -10.f), -2.f, 3.f, 0, &rng));
  const std::vector<ProjectedSplat> projected = projectSplats(cloud, makeView(), {});
  EXPECT_EQ(1u, projected.size());
}

TEST(SplatRaster, EmptyAreasShowTheBackground) {
  GaussianCloud cloud;
  RasterOptions options;
  options.background = {0.25f, 0.5f, 0.75f};
  const SplatImage image = renderSplats(cloud, makeView(), options);
  ASSERT_EQ(48 * 36 * 3u, image.rgb.size());
  for (size_t p = 0; p < image.transmittance.size(); ++p) {
    EXPECT_FLOAT_EQ(1.f, image.transmittance[p]);
    EXPECT_FLOAT_EQ(0.25f, image.rgb[p * 3 + 0]);
    EXPECT_FLOAT_EQ(0.5f, image.rgb[p * 3 + 1]);
    EXPECT_FLOAT_EQ(0.75f, image.rgb[p * 3 + 2]);
  }
}

TEST(SplatRaster, ANearOpaqueGaussianHidesTheOneBehindIt) {
  const SplatView view = makeView();
  Gaussian front;
  front.position = Vec3f(0.f, 0.f, -0.5f);
  front.log_scale = Vec3f::Constant(std::log(0.4f));
  front.opacity_logit = 8.f;  // sigmoid is within a thousandth of one
  front.dc = {(1.f - 0.5f) / kShC0, -0.5f / kShC0, -0.5f / kShC0};  // pure red

  Gaussian back = front;
  back.position = Vec3f(0.f, 0.f, 0.5f);
  back.dc = {-0.5f / kShC0, -0.5f / kShC0, (1.f - 0.5f) / kShC0};  // pure blue

  GaussianCloud cloud;
  cloud.gaussians = {back, front};
  const SplatImage image = renderSplats(cloud, view, {});
  const int cx = view.intrinsics.width / 2;
  const int cy = view.intrinsics.height / 2;
  EXPECT_GT(image.at(cx, cy, 0), 0.9f);
  EXPECT_LT(image.at(cx, cy, 2), 0.1f);

  // Swapping the depths swaps which colour wins, and nothing else changes.
  std::swap(cloud.gaussians[0].position, cloud.gaussians[1].position);
  const SplatImage swapped = renderSplats(cloud, view, {});
  EXPECT_LT(swapped.at(cx, cy, 0), 0.1f);
  EXPECT_GT(swapped.at(cx, cy, 2), 0.9f);
}

TEST(SplatRaster, OpacityControlsHowMuchBackgroundSurvives) {
  const SplatView view = makeView();
  GaussianCloud cloud;
  Gaussian g;
  g.position = Vec3f::Zero();
  g.log_scale = Vec3f::Constant(std::log(0.4f));
  g.dc = {0.f, 0.f, 0.f};
  cloud.gaussians.push_back(g);
  RasterOptions options;
  options.background = {1.f, 1.f, 1.f};

  const int cx = view.intrinsics.width / 2;
  const int cy = view.intrinsics.height / 2;
  float previous = 2.f;
  for (float logit : {-2.f, 0.f, 2.f, 4.f}) {
    cloud.gaussians[0].opacity_logit = logit;
    const SplatImage image = renderSplats(cloud, view, options);
    const float value = image.at(cx, cy, 0);
    EXPECT_LT(value, previous);
    previous = value;
  }
}

TEST(SplatRaster, IdenticalImagesHaveUnitSimilarity) {
  const std::vector<float> image = makeTarget(32 * 24 * 3, 11);
  EXPECT_NEAR(1.0f, meanSsim(image, image, 32, 24), 1e-5f);
}

TEST(SplatRaster, BlurringAnImageLowersItsSimilarity) {
  const int width = 32, height = 24;
  std::vector<float> sharp(static_cast<size_t>(width) * height * 3, 0.f);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float value = ((x / 4 + y / 4) % 2 == 0) ? 1.f : 0.f;
      for (int c = 0; c < 3; ++c) sharp[(static_cast<size_t>(y) * width + x) * 3 + c] = value;
    }
  }
  std::vector<float> flat(sharp.size(), 0.5f);
  EXPECT_LT(meanSsim(flat, sharp, width, height), 0.5f);
  EXPECT_GT(meanSsim(sharp, sharp, width, height), 0.99f);
}

TEST(SplatRaster, SsimGradientMatchesFiniteDifferences) {
  const int width = 16, height = 12;
  std::vector<float> rendered = makeTarget(static_cast<size_t>(width) * height * 3, 21);
  const std::vector<float> target = makeTarget(static_cast<size_t>(width) * height * 3, 22);

  std::vector<float> analytic;
  meanSsim(rendered, target, width, height, &analytic);

  Rng rng(23);
  for (int trial = 0; trial < 20; ++trial) {
    const size_t i = static_cast<size_t>(rng.uniform(0.0, static_cast<double>(rendered.size())));
    const float original = rendered[i];
    const double step = 1e-3;
    rendered[i] = static_cast<float>(original + step);
    const double plus = meanSsim(rendered, target, width, height);
    rendered[i] = static_cast<float>(original - step);
    const double minus = meanSsim(rendered, target, width, height);
    rendered[i] = original;
    EXPECT_LT(relativeError(analytic[i], (plus - minus) / (2.0 * step)), 2e-2)
        << "value " << i;
  }
}

TEST(SplatRaster, PhotometricLossCombinesBothTermsAndIsZeroOnAPerfectMatch) {
  SplatImage image;
  image.width = 16;
  image.height = 12;
  image.rgb = makeTarget(16 * 12 * 3, 31);
  image.transmittance.assign(16 * 12, 0.f);
  EXPECT_NEAR(0.f, photometricLoss(image, image.rgb, 0.2f, nullptr), 1e-4f);

  const std::vector<float> other = makeTarget(16 * 12 * 3, 32);
  const float l1_only = photometricLoss(image, other, 0.f, nullptr);
  const float mixed = photometricLoss(image, other, 0.2f, nullptr);
  EXPECT_GT(l1_only, 0.f);
  EXPECT_NE(l1_only, mixed);
}

TEST(SplatRaster, BackwardPassMatchesFiniteDifferencesForEveryParameter) {
  // One tile exactly. Tile membership is decided from the splat's radius, so
  // on a larger image a perturbation can push a splat's bounding box across a
  // tile boundary and add or drop that tile's contribution in one step -- a
  // real discontinuity in the rasteriser, and not one the gradient describes.
  // Keeping everything inside a single tile removes it from the check.
  const SplatView view = makeView(16, 16);
  RasterOptions options;
  options.sh_degree = 2;
  options.background = {0.2f, 0.3f, 0.4f};
  // The alpha floor and the transmittance cutoff are deliberate
  // discontinuities in the forward pass: a perturbation that moves a pixel
  // across one of them changes the finite difference by a step the analytic
  // gradient cannot see. They are speed optimisations, so the check runs
  // without them and the maths is tested rather than the thresholds.
  options.min_alpha = 0.f;
  options.transmittance_cutoff = 0.f;
  GaussianCloud cloud = makeOverlappingCloud(options.sh_degree);
  std::vector<float> weights =
      makeTarget(static_cast<size_t>(view.intrinsics.width) * view.intrinsics.height * 3, 41);
  for (float& w : weights) w -= 0.5f;  // both signs, so nothing cancels by luck

  const auto loss = [&]() { return evaluateLinear(cloud, view, options, weights, false).loss; };
  const LossAndGradients analytic = evaluateLinear(cloud, view, options, weights, true);
  ASSERT_EQ(cloud.size(), analytic.gradients.position.size());

  const int rest_stride = static_cast<int>(cloud.gaussians.front().sh_rest.size());
  for (size_t g = 0; g < cloud.size(); ++g) {
    Gaussian& gaussian = cloud.gaussians[g];

    for (int a = 0; a < 3; ++a) {
      const double numeric = numericGradient(
          [&](float v) { gaussian.position[a] = v; }, gaussian.position[a], 2e-3, loss);
      EXPECT_LT(relativeError(analytic.gradients.position[g][a], numeric), 5e-2)
          << "position[" << g << "][" << a << "]";
    }
    for (int a = 0; a < 3; ++a) {
      const double numeric = numericGradient(
          [&](float v) { gaussian.log_scale[a] = v; }, gaussian.log_scale[a], 2e-3, loss);
      EXPECT_LT(relativeError(analytic.gradients.log_scale[g][a], numeric), 5e-2)
          << "log_scale[" << g << "][" << a << "]";
    }
    {
      const double numeric = numericGradient(
          [&](float v) { gaussian.opacity_logit = v; }, gaussian.opacity_logit, 2e-3, loss);
      EXPECT_LT(relativeError(analytic.gradients.opacity_logit[g], numeric), 5e-2)
          << "opacity[" << g << "]";
    }
    for (int c = 0; c < 3; ++c) {
      const double numeric =
          numericGradient([&](float v) { gaussian.dc[c] = v; }, gaussian.dc[c], 2e-3, loss);
      EXPECT_LT(relativeError(analytic.gradients.dc[g][c], numeric), 5e-2)
          << "dc[" << g << "][" << c << "]";
    }
    // The quaternion is normalised inside the rasteriser, so perturbing a raw
    // component moves the rotation along the sphere's tangent; the analytic
    // gradient is taken with respect to the same raw components.
    for (int a = 0; a < 4; ++a) {
      float* component = a == 0 ? &gaussian.rotation.w()
                                : (a == 1 ? &gaussian.rotation.x()
                                          : (a == 2 ? &gaussian.rotation.y()
                                                    : &gaussian.rotation.z()));
      const double numeric = numericGradient([&](float v) { *component = v; }, *component, 2e-3,
                                             loss);
      EXPECT_LT(relativeError(analytic.gradients.rotation[g][a], numeric), 8e-2)
          << "rotation[" << g << "][" << a << "]";
    }
    for (int k = 0; k < rest_stride; k += 2) {
      const double numeric = numericGradient([&](float v) { gaussian.sh_rest[k] = v; },
                                             gaussian.sh_rest[k], 2e-3, loss);
      EXPECT_LT(relativeError(analytic.gradients.sh_rest[g * rest_stride + k], numeric), 5e-2)
          << "sh_rest[" << g << "][" << k << "]";
    }
  }
}

TEST(SplatRaster, BackwardPassAccumulatesAcrossViewsRatherThanOverwriting) {
  RasterOptions options;
  GaussianCloud cloud = makeOverlappingCloud(0, 3);
  const SplatView view = makeView(24, 20);
  const std::vector<float> target = makeTarget(24 * 20 * 3, 51);

  const std::vector<ProjectedSplat> projected = projectSplats(cloud, view, options);
  const SplatImage image = rasterizeSplats(projected, view, options);
  std::vector<float> dL_drgb;
  photometricLoss(image, target, 0.f, &dL_drgb);

  SplatGradients once;
  once.resize(cloud.size(), static_cast<int>(cloud.gaussians.front().sh_rest.size()));
  rasterizeSplatsBackward(cloud, view, options, projected, image, dL_drgb, &once);

  SplatGradients twice = once;
  rasterizeSplatsBackward(cloud, view, options, projected, image, dL_drgb, &twice);

  for (size_t g = 0; g < cloud.size(); ++g) {
    EXPECT_NEAR(2.f * once.opacity_logit[g], twice.opacity_logit[g], 1e-5f);
    EXPECT_EQ(2 * once.visible_count[g], twice.visible_count[g]);
    for (int a = 0; a < 3; ++a) {
      EXPECT_NEAR(2.f * once.position[g][a], twice.position[g][a], 1e-5f);
    }
  }
}

TEST(SplatRaster, ScreenGradientIsLargerForASplatTheLossIsFightingWith) {
  const SplatView view = makeView(48, 32);
  GaussianCloud cloud;
  for (float x : {-0.5f, 0.5f}) {
    Gaussian g;
    g.position = Vec3f(x, 0.f, 0.f);
    g.log_scale = Vec3f::Constant(std::log(0.1f));
    g.opacity_logit = 2.f;
    g.dc = {1.f, 1.f, 1.f};
    cloud.gaussians.push_back(g);
  }

  // The target is exactly what this cloud renders, so at this point the loss
  // has nothing to pull on and every screen gradient is essentially zero.
  const std::vector<float> target = renderSplats(cloud, view, {}).rgb;
  const LossAndGradients settled = evaluate(cloud, view, {}, target, 0.f, true);
  EXPECT_LT(settled.gradients.screen_gradient[0], 1e-6f);
  EXPECT_LT(settled.gradients.screen_gradient[1], 1e-6f);

  // Move one of them off its target and the loss starts dragging it back.
  cloud.gaussians[1].position.x() += 0.08f;
  const LossAndGradients disturbed = evaluate(cloud, view, {}, target, 0.f, true);
  EXPECT_GT(disturbed.gradients.screen_gradient[1],
            10.f * std::max(disturbed.gradients.screen_gradient[0], 1e-9f));
  EXPECT_EQ(1, disturbed.gradients.visible_count[0]);
  EXPECT_EQ(1, disturbed.gradients.visible_count[1]);
}
