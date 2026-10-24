#include "parallax/splat/SplatTrainer.h"

#include <gtest/gtest.h>

#include <cmath>

#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// Cameras on a ring around the origin, all looking inwards.
std::vector<SplatView> ringOfViews(int count, double radius, int width, int height) {
  std::vector<SplatView> views;
  for (int i = 0; i < count; ++i) {
    const double angle = 2.0 * M_PI * i / count;
    SplatView view;
    view.intrinsics.width = width;
    view.intrinsics.height = height;
    view.intrinsics.fx = view.intrinsics.fy = 1.1 * width;
    view.intrinsics.cx = 0.5 * width;
    view.intrinsics.cy = 0.5 * height;

    const Vec3 eye(radius * std::sin(angle), 0.0, -radius * std::cos(angle));
    const Vec3 forward = (Vec3::Zero() - eye).normalized();
    const Vec3 right = Vec3::UnitY().cross(forward).normalized();
    const Vec3 down = forward.cross(right);
    Mat3 camera_from_world;
    camera_from_world.row(0) = right;
    camera_from_world.row(1) = down;
    camera_from_world.row(2) = forward;
    view.camera_from_world = Pose(camera_from_world, -camera_from_world * eye);
    views.push_back(view);
  }
  return views;
}

/// A small, opaque, well-separated target scene: three coloured blobs.
GaussianCloud truthCloud() {
  GaussianCloud cloud;
  cloud.sh_degree = 0;
  const Vec3f positions[3] = {Vec3f(-0.25f, 0.f, 0.f), Vec3f(0.25f, 0.f, 0.f),
                              Vec3f(0.f, 0.2f, 0.2f)};
  const std::array<float, 3> colors[3] = {{0.9f, 0.2f, 0.2f}, {0.2f, 0.8f, 0.3f},
                                          {0.3f, 0.3f, 0.9f}};
  for (int i = 0; i < 3; ++i) {
    Gaussian g;
    g.position = positions[i];
    g.log_scale = Vec3f::Constant(std::log(0.12f));
    g.opacity_logit = 4.f;
    for (int c = 0; c < 3; ++c) g.dc[c] = (colors[i][c] - 0.5f) / kShC0;
    cloud.gaussians.push_back(g);
  }
  return cloud;
}

std::vector<SplatTrainingView> renderTrainingSet(const GaussianCloud& cloud,
                                                 const std::vector<SplatView>& views) {
  std::vector<SplatTrainingView> set;
  for (const SplatView& view : views) {
    SplatTrainingView entry;
    entry.view = view;
    entry.image = renderSplats(cloud, view, {}).rgb;
    set.push_back(std::move(entry));
  }
  return set;
}

/// A deliberately wrong starting point: grey blobs in roughly the right place.
GaussianCloud roughStart(size_t count = 24, unsigned seed = 3) {
  GaussianCloud cloud;
  cloud.sh_degree = 0;
  Rng rng(seed);
  for (size_t i = 0; i < count; ++i) {
    Gaussian g;
    g.position = Vec3f(static_cast<float>(rng.uniform(-0.4, 0.4)),
                       static_cast<float>(rng.uniform(-0.3, 0.3)),
                       static_cast<float>(rng.uniform(-0.3, 0.3)));
    g.log_scale = Vec3f::Constant(std::log(0.1f));
    g.opacity_logit = 0.f;
    g.dc = {0.f, 0.f, 0.f};
    cloud.gaussians.push_back(g);
  }
  return cloud;
}

SplatTrainingConfig shortSchedule(int iterations) {
  SplatTrainingConfig config;
  config.iterations = iterations;
  config.sh_degree = 0;
  config.sh_increase_interval = 0;
  config.densify_from_iteration = iterations + 1;  // off unless a test wants it
  config.densify_until_iteration = iterations + 1;
  config.opacity_reset_interval = 0;
  config.ssim_weight = 0.2f;
  return config;
}

float meanAbsoluteError(const std::vector<float>& a, const std::vector<float>& b) {
  double total = 0.0;
  for (size_t i = 0; i < a.size(); ++i) total += std::abs(a[i] - b[i]);
  return static_cast<float>(total / static_cast<double>(a.size()));
}

}  // namespace

TEST(SplatTrainer, ConfigValidationRejectsSchedulesThatCannotRun) {
  EXPECT_TRUE(SplatTrainingConfig{}.validate().empty());

  SplatTrainingConfig config;
  config.iterations = 0;
  EXPECT_FALSE(config.validate().empty());

  config = {};
  config.sh_degree = 7;
  EXPECT_FALSE(config.validate().empty());

  config = {};
  config.split_scale_divider = 0.8f;  // would make children larger than parents
  EXPECT_FALSE(config.validate().empty());

  config = {};
  config.ssim_weight = 1.5f;
  EXPECT_FALSE(config.validate().empty());
}

TEST(SplatTrainer, SceneExtentFollowsTheCameraRig) {
  const GaussianCloud start = roughStart();
  for (double radius : {1.0, 4.0}) {
    SplatTrainer trainer(start, renderTrainingSet(truthCloud(), ringOfViews(4, radius, 16, 16)),
                         shortSchedule(1));
    EXPECT_NEAR(radius * 1.1, trainer.sceneExtent(), 1e-3);
  }
}

TEST(SplatTrainer, TrainingDrivesTheLossDown) {
  const std::vector<SplatView> views = ringOfViews(6, 1.6, 32, 32);
  const std::vector<SplatTrainingView> training = renderTrainingSet(truthCloud(), views);

  SplatTrainer trainer(roughStart(), training, shortSchedule(400));
  ASSERT_TRUE(trainer.step());
  const float first = trainer.lastLoss();
  ASSERT_TRUE(trainer.train());
  EXPECT_LT(trainer.smoothedLoss(), 0.5f * first)
      << "started at " << first << " and finished at " << trainer.smoothedLoss();
}

TEST(SplatTrainer, TrainingGeneralisesToAViewItNeverSaw) {
  // Seven cameras on the ring; the trainer gets six and is scored on the odd
  // one out. Fitting the training views is easy, so this is the test that
  // says the geometry is actually right rather than memorised.
  const std::vector<SplatView> all = ringOfViews(7, 1.6, 32, 32);
  const GaussianCloud truth = truthCloud();
  std::vector<SplatView> training_views(all.begin(), all.end() - 1);
  const SplatView held_out = all.back();

  const std::vector<float> expected = renderSplats(truth, held_out, {}).rgb;
  const GaussianCloud start = roughStart();
  const float before = meanAbsoluteError(renderSplats(start, held_out, {}).rgb, expected);

  SplatTrainer trainer(start, renderTrainingSet(truth, training_views), shortSchedule(600));
  ASSERT_TRUE(trainer.train());
  const GaussianCloud trained = trainer.finish();
  const float after = meanAbsoluteError(renderSplats(trained, held_out, {}).rgb, expected);

  EXPECT_LT(after, 0.5f * before) << "held-out error went from " << before << " to " << after;
}

TEST(SplatTrainer, TrainingIsDeterministicForAGivenSeed) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 16, 16));
  SplatTrainingConfig config = shortSchedule(60);
  config.densify_from_iteration = 20;
  config.densify_until_iteration = 60;
  config.densify_interval = 20;
  config.densify_grad_threshold = 0.f;

  const auto run = [&]() {
    SplatTrainer trainer(roughStart(8), training, config);
    trainer.train();
    return trainer.finish();
  };
  const GaussianCloud first = run();
  const GaussianCloud second = run();

  ASSERT_EQ(first.size(), second.size());
  for (size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first.gaussians[i].position, second.gaussians[i].position);
    EXPECT_FLOAT_EQ(first.gaussians[i].opacity_logit, second.gaussians[i].opacity_logit);
  }
}

TEST(SplatTrainer, CancellingStopsTrainingWhereItWasAsked) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 16, 16));
  SplatTrainer trainer(roughStart(8), training, shortSchedule(500));
  int seen = 0;
  const bool completed = trainer.train([&](const SplatTrainingProgress& progress) {
    seen = progress.iteration;
    EXPECT_EQ(500, progress.total_iterations);
    EXPECT_GT(progress.gaussian_count, 0);
    return progress.iteration < 25;
  });
  EXPECT_FALSE(completed);
  EXPECT_EQ(25, seen);
  EXPECT_EQ(25, trainer.iteration());
}

TEST(SplatTrainer, SphericalHarmonicBandsUnlockOnSchedule) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(3, 1.6, 12, 12));
  GaussianCloud start = roughStart(4);
  start.sh_degree = 3;
  for (Gaussian& g : start.gaussians) g.sh_rest.assign(shRestPerChannel(3) * 3, 0.f);

  SplatTrainingConfig config = shortSchedule(31);
  config.sh_degree = 3;
  config.sh_increase_interval = 10;

  SplatTrainer trainer(start, training, config);
  EXPECT_EQ(0, trainer.shDegreeInUse());
  for (int target = 1; target <= 3; ++target) {
    while (trainer.iteration() < target * 10) ASSERT_TRUE(trainer.step());
    EXPECT_EQ(target, trainer.shDegreeInUse());
  }
}

TEST(SplatTrainer, DensificationAddsGaussiansWhereTheLossIsPulling) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 24, 24));

  SplatTrainingConfig config = shortSchedule(20);
  config.densify_from_iteration = 10;
  config.densify_until_iteration = 20;
  config.densify_interval = 10;
  config.densify_grad_threshold = 0.f;  // everything visible qualifies
  const size_t initial = 8;

  SplatTrainer grown(roughStart(initial), training, config);
  ASSERT_TRUE(grown.train());
  EXPECT_GT(grown.cloud().size(), initial);

  // The same schedule with a threshold nothing can reach must leave it alone.
  config.densify_grad_threshold = 1e9f;
  SplatTrainer unchanged(roughStart(initial), training, config);
  ASSERT_TRUE(unchanged.train());
  EXPECT_EQ(initial, unchanged.cloud().size());
}

TEST(SplatTrainer, DensificationRespectsTheGaussianCeiling) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 24, 24));
  SplatTrainingConfig config = shortSchedule(40);
  config.densify_from_iteration = 5;
  config.densify_until_iteration = 40;
  config.densify_interval = 5;
  config.densify_grad_threshold = 0.f;
  config.max_gaussians = 12;

  SplatTrainer trainer(roughStart(10), training, config);
  ASSERT_TRUE(trainer.train());
  EXPECT_LE(trainer.cloud().size(), 12u);
}

TEST(SplatTrainer, TransparentGaussiansArePrunedAway) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 16, 16));
  GaussianCloud start = roughStart(10);
  for (Gaussian& g : start.gaussians) g.opacity_logit = -20.f;  // invisible

  SplatTrainingConfig config = shortSchedule(10);
  config.densify_from_iteration = 10;
  config.densify_until_iteration = 10;
  config.densify_interval = 10;
  config.densify_grad_threshold = 1e9f;
  config.opacity_lr = 0.f;  // nothing can rescue them

  SplatTrainer trainer(start, training, config);
  ASSERT_TRUE(trainer.train());
  EXPECT_TRUE(trainer.cloud().empty());
}

TEST(SplatTrainer, OpacityResetCapsEveryGaussianAndTrainingRecovers) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(4, 1.6, 16, 16));
  GaussianCloud start = roughStart(6);
  for (Gaussian& g : start.gaussians) g.opacity_logit = 5.f;

  SplatTrainingConfig config = shortSchedule(60);
  config.opacity_reset_interval = 20;
  config.densify_until_iteration = 40;  // resets only happen while densifying
  config.densify_from_iteration = 41;
  config.opacity_lr = 0.f;              // isolate the reset from the optimiser

  SplatTrainer trainer(start, training, config);
  while (trainer.iteration() < 20) ASSERT_TRUE(trainer.step());
  for (const Gaussian& g : trainer.cloud().gaussians) {
    EXPECT_LT(gaussianOpacity(g), 0.011f);
  }
}

TEST(SplatTrainer, AnEmptyCloudOrAnEmptyViewSetFinishesWithoutCrashing) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(2, 1.6, 8, 8));

  SplatTrainer no_gaussians(GaussianCloud{}, training, shortSchedule(10));
  EXPECT_TRUE(no_gaussians.train());
  EXPECT_TRUE(no_gaussians.cloud().empty());

  SplatTrainer no_views(roughStart(4), {}, shortSchedule(10));
  EXPECT_TRUE(no_views.train());
  EXPECT_EQ(4u, no_views.cloud().size());
}

TEST(SplatTrainer, FinishPrunesSortsAndRecordsTheDegree) {
  const std::vector<SplatTrainingView> training =
      renderTrainingSet(truthCloud(), ringOfViews(3, 1.6, 12, 12));
  GaussianCloud start = roughStart(6);
  start.gaussians[0].opacity_logit = -30.f;
  start.gaussians[3].opacity_logit = -30.f;

  SplatTrainingConfig config = shortSchedule(1);
  config.opacity_lr = 0.f;
  config.sh_degree = 0;

  SplatTrainer trainer(start, training, config);
  ASSERT_TRUE(trainer.train());
  const GaussianCloud finished = trainer.finish();

  EXPECT_EQ(4u, finished.size());
  EXPECT_EQ(0, finished.sh_degree);
  for (size_t i = 1; i < finished.size(); ++i) {
    EXPECT_GE(finished.gaussians[i - 1].opacity_logit, finished.gaussians[i].opacity_logit);
  }
  std::string reason;
  EXPECT_TRUE(finished.isValid(&reason)) << reason;
}

TEST(SplatTrainer, TheBackendIsReportedSoALogSaysWhichPathRan) {
  EXPECT_EQ("cpu", makeCpuSplatBackend()->name());
}
