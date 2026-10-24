#include "parallax/sfm/BundleAdjustment.h"

#include <gtest/gtest.h>

#include "parallax/sfm/Pnp.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A synthetic reconstruction: cameras on an arc, landmarks filling the volume.
struct Problem {
  BundleProblem bundle;
  std::vector<Pose> true_poses;
  std::vector<Vec3> true_points;
};

Problem makeProblem(int cameras = 6, int points = 90, uint64_t seed = 2024,
                    double noise_px = 0.0) {
  Problem p;
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Vec3 target(0, 0, 0);
  p.true_poses = SyntheticScene::orbitPoses(target, 2.2, 0.3, cameras, 0.0, deg2rad(110.0));

  Rng rng(seed);
  while (static_cast<int>(p.true_points.size()) < points) {
    const Vec3 x(rng.uniform(-0.8, 0.8), rng.uniform(-0.6, 0.6), rng.uniform(-0.8, 0.8));
    // Keep only landmarks visible from at least three cameras.
    int seen = 0;
    for (const Pose& pose : p.true_poses) {
      Vec2 px;
      if (!k.project(pose * x, &px)) continue;
      if (px.x() > 3 && px.y() > 3 && px.x() < k.width - 4 && px.y() < k.height - 4) ++seen;
    }
    if (seen >= 3) p.true_points.push_back(x);
  }

  p.bundle.poses = p.true_poses;
  p.bundle.points = p.true_points;
  p.bundle.intrinsics.assign(p.true_poses.size(), k);
  for (size_t c = 0; c < p.true_poses.size(); ++c) {
    for (size_t j = 0; j < p.true_points.size(); ++j) {
      Vec2 px;
      if (!k.project(p.true_poses[c] * p.true_points[j], &px)) continue;
      if (px.x() <= 3 || px.y() <= 3 || px.x() >= k.width - 4 || px.y() >= k.height - 4) continue;
      if (noise_px > 0.0) px += Vec2(rng.normal(), rng.normal()) * noise_px;
      p.bundle.observations.push_back(
          Observation{static_cast<int>(c), static_cast<int>(j), px, 1.0f});
    }
  }
  return p;
}

/// Mean distance between corresponding points of two clouds.
double cloudError(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) sum += (a[i] - b[i]).norm();
  return a.empty() ? 0.0 : sum / a.size();
}

std::vector<PointCorrespondence> correspondencesFor(const Pose& pose, const CameraIntrinsics& k,
                                                    const std::vector<Vec3>& points) {
  std::vector<PointCorrespondence> out;
  for (const Vec3& x : points) {
    Vec2 px;
    if (!k.project(pose * x, &px)) continue;
    if (px.x() < 0 || px.y() < 0 || px.x() > k.width || px.y() > k.height) continue;
    out.push_back(PointCorrespondence{x, px});
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------- jacobians ---

TEST(ProjectionJacobian, MatchesFiniteDifferencesForPose) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose pose = SyntheticScene::lookAt(Vec3(0.4, 0.3, -1.5), Vec3(0, 0, 0));
  Rng rng(1);

  for (int trial = 0; trial < 40; ++trial) {
    const Vec3 x(rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5));
    Vec2 px;
    Eigen::Matrix<double, 2, 6> J;
    ASSERT_TRUE(projectionJacobian(pose, k, x, &px, &J, nullptr));

    // Central differences on the same left-multiplied increment the solver uses.
    const double h = 1e-7;
    for (int c = 0; c < 6; ++c) {
      Eigen::Matrix<double, 6, 1> d = Eigen::Matrix<double, 6, 1>::Zero();
      d(c) = h;
      Pose plus = pose, minus = pose;
      plus.applyLeftIncrement(d);
      minus.applyLeftIncrement(-d);
      Vec2 p_plus, p_minus;
      ASSERT_TRUE(k.project(plus * x, &p_plus));
      ASSERT_TRUE(k.project(minus * x, &p_minus));
      const Vec2 numeric = (p_plus - p_minus) / (2.0 * h);
      EXPECT_LT((numeric - J.col(c)).norm(), 1e-3 * (1.0 + J.col(c).norm()))
          << "pose column " << c;
    }
  }
}

TEST(ProjectionJacobian, MatchesFiniteDifferencesForPoint) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose pose = SyntheticScene::lookAt(Vec3(-0.6, 0.2, -2.0), Vec3(0, 0, 0));
  Rng rng(2);

  for (int trial = 0; trial < 40; ++trial) {
    const Vec3 x(rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5));
    Vec2 px;
    Eigen::Matrix<double, 2, 3> Jp;
    ASSERT_TRUE(projectionJacobian(pose, k, x, &px, nullptr, &Jp));

    const double h = 1e-7;
    for (int c = 0; c < 3; ++c) {
      Vec3 dx = Vec3::Zero();
      dx(c) = h;
      Vec2 p_plus, p_minus;
      ASSERT_TRUE(k.project(pose * (x + dx), &p_plus));
      ASSERT_TRUE(k.project(pose * (x - dx), &p_minus));
      const Vec2 numeric = (p_plus - p_minus) / (2.0 * h);
      EXPECT_LT((numeric - Jp.col(c)).norm(), 1e-3 * (1.0 + Jp.col(c).norm()))
          << "point column " << c;
    }
  }
}

TEST(ProjectionJacobian, RejectsPointsBehindTheCamera) {
  const CameraIntrinsics k = phoneIntrinsics();
  Vec2 px;
  Eigen::Matrix<double, 2, 6> J;
  EXPECT_FALSE(projectionJacobian(Pose::Identity(), k, Vec3(0, 0, -1), &px, &J, nullptr));
}

// --------------------------------------------------------------------- pnp ---

TEST(Pnp, DltRecoversPoseExactlyWithoutNoise) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose truth = SyntheticScene::lookAt(Vec3(1.1, 0.7, -1.9), Vec3(0.05, 0, 0.1));
  Rng rng(7);
  std::vector<Vec3> points;
  for (int i = 0; i < 40; ++i) {
    points.emplace_back(rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7));
  }
  const auto correspondences = correspondencesFor(truth, k, points);
  ASSERT_GE(correspondences.size(), 10u);

  Pose estimated;
  ASSERT_TRUE(pnpDlt(correspondences, k, &estimated));
  EXPECT_LT(rad2deg(Pose::rotationAngle(estimated, truth)), 1e-6);
  EXPECT_LT((estimated.center() - truth.center()).norm(), 1e-7);
}

TEST(Pnp, DltRejectsTooFewPoints) {
  Pose p;
  EXPECT_FALSE(pnpDlt({}, phoneIntrinsics(), &p));
  std::vector<PointCorrespondence> five(5);
  EXPECT_FALSE(pnpDlt(five, phoneIntrinsics(), &p));
}

TEST(Pnp, RansacRejectsOutliers) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose truth = SyntheticScene::lookAt(Vec3(0.9, 0.5, -2.1), Vec3(0, 0, 0));
  Rng rng(11);
  std::vector<Vec3> points;
  for (int i = 0; i < 120; ++i) {
    points.emplace_back(rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8));
  }
  auto correspondences = correspondencesFor(truth, k, points);
  ASSERT_GE(correspondences.size(), 60u);

  // Corrupt a third of the pixel observations.
  const size_t corrupt = correspondences.size() / 3;
  for (size_t i = 0; i < corrupt; ++i) {
    correspondences[i].pixel = Vec2(rng.uniform(0, k.width), rng.uniform(0, k.height));
  }

  RansacOptions options;
  options.inlier_threshold = 3.0;
  options.max_iterations = 3000;
  const PnpResult result = pnpRansac(correspondences, k, options);
  ASSERT_TRUE(result.success);
  EXPECT_LT(rad2deg(Pose::rotationAngle(result.pose, truth)), 1.0);
  EXPECT_LT((result.pose.center() - truth.center()).norm(), 0.05);

  int rejected = 0;
  for (size_t i = 0; i < corrupt; ++i) {
    if (!result.inliers[i]) ++rejected;
  }
  EXPECT_GT(static_cast<double>(rejected) / corrupt, 0.9);
}

TEST(Pnp, GaussNewtonImprovesAPerturbedPose) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose truth = SyntheticScene::lookAt(Vec3(0.8, 0.4, -2.0), Vec3(0, 0, 0));
  Rng rng(13);
  std::vector<Vec3> points;
  for (int i = 0; i < 60; ++i) {
    points.emplace_back(rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7));
  }
  const auto correspondences = correspondencesFor(truth, k, points);

  Pose perturbed = truth;
  Eigen::Matrix<double, 6, 1> d;
  d << 0.04, -0.03, 0.05, 0.02, -0.025, 0.015;
  perturbed.applyLeftIncrement(d);
  const double before = rad2deg(Pose::rotationAngle(perturbed, truth));

  double rmse = 0.0;
  ASSERT_TRUE(refinePoseGaussNewton(correspondences, k, &perturbed, 30, 2.5, &rmse));
  const double after = rad2deg(Pose::rotationAngle(perturbed, truth));
  EXPECT_LT(after, before);
  EXPECT_LT(after, 1e-4);
  EXPECT_LT(rmse, 1e-4);
}

TEST(Pnp, GaussNewtonIsRobustToASmallFractionOfMismatches) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose truth = SyntheticScene::lookAt(Vec3(0.7, 0.35, -2.2), Vec3(0, 0, 0));
  Rng rng(17);
  std::vector<Vec3> points;
  for (int i = 0; i < 100; ++i) {
    points.emplace_back(rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7));
  }
  auto correspondences = correspondencesFor(truth, k, points);
  // 10% gross errors: the Huber loss should cap their influence.
  for (size_t i = 0; i < correspondences.size() / 10; ++i) {
    correspondences[i].pixel += Vec2(90.0, -70.0);
  }

  Pose estimate = truth;
  Eigen::Matrix<double, 6, 1> d;
  d << 0.02, 0.01, -0.02, 0.01, 0.008, -0.006;
  estimate.applyLeftIncrement(d);
  ASSERT_TRUE(refinePoseGaussNewton(correspondences, k, &estimate, 40, 2.0, nullptr));
  EXPECT_LT(rad2deg(Pose::rotationAngle(estimate, truth)), 0.5);
}

// -------------------------------------------------------- bundle adjustment ---

TEST(Bundle, ValidatesItsInput) {
  BundleProblem empty;
  EXPECT_FALSE(empty.valid());
  BundleSummary s = optimizeBundle(&empty);
  EXPECT_FALSE(s.success);
  EXPECT_EQ(optimizeBundle(nullptr).success, false);

  Problem p = makeProblem(3, 20);
  // Out-of-range observation indices must be caught, not dereferenced.
  p.bundle.observations.push_back(Observation{99, 0, Vec2(1, 1), 1.f});
  EXPECT_FALSE(p.bundle.valid());
}

TEST(Bundle, LeavesAPerfectSolutionAlone) {
  Problem p = makeProblem(6, 80);
  const double rmse_before = bundleRmsePixels(p.bundle);
  EXPECT_LT(rmse_before, 1e-9) << "synthetic data should start exact";

  BundleOptions options;
  options.max_iterations = 20;
  const BundleSummary summary = optimizeBundle(&p.bundle, options);
  EXPECT_LT(bundleRmsePixels(p.bundle), 1e-6);
  EXPECT_LT(cloudError(p.bundle.points, p.true_points), 1e-6);
  EXPECT_TRUE(summary.success) << summary.message;
  // Starting at the optimum is convergence, not failure, and must not be
  // reported as progress.
  EXPECT_TRUE(summary.converged) << summary.message;
  EXPECT_LE(summary.final_cost, summary.initial_cost);
}

TEST(Bundle, RecoversFromPerturbedPosesAndPoints) {
  Problem p = makeProblem(8, 120, 99);
  Rng rng(555);

  // Perturb everything except the first camera, which the gauge fixes.
  for (size_t i = 1; i < p.bundle.poses.size(); ++i) {
    Eigen::Matrix<double, 6, 1> d;
    for (int c = 0; c < 3; ++c) d(c) = rng.normal() * 0.03;
    for (int c = 3; c < 6; ++c) d(c) = rng.normal() * 0.012;
    p.bundle.poses[i].applyLeftIncrement(d);
  }
  for (Vec3& x : p.bundle.points) x += Vec3(rng.normal(), rng.normal(), rng.normal()) * 0.02;

  const double rmse_before = bundleRmsePixels(p.bundle);
  const double cloud_before = cloudError(p.bundle.points, p.true_points);
  EXPECT_GT(rmse_before, 1.0) << "perturbation should be visible in pixels";

  BundleOptions options;
  options.max_iterations = 80;
  const BundleSummary summary = optimizeBundle(&p.bundle, options);

  EXPECT_TRUE(summary.success) << summary.message;
  EXPECT_LT(summary.final_rmse_px, 0.05) << summary.message;
  EXPECT_LT(summary.final_rmse_px, rmse_before);
  EXPECT_LT(summary.final_cost, summary.initial_cost);
  EXPECT_LT(cloudError(p.bundle.points, p.true_points), cloud_before);
  EXPECT_LT(cloudError(p.bundle.points, p.true_points), 0.01);
  EXPECT_GT(summary.successful_steps, 0);
}

TEST(Bundle, CostDecreasesMonotonically) {
  Problem p = makeProblem(6, 70, 31);
  Rng rng(32);
  for (size_t i = 1; i < p.bundle.poses.size(); ++i) {
    Eigen::Matrix<double, 6, 1> d;
    for (int c = 0; c < 6; ++c) d(c) = rng.normal() * 0.02;
    p.bundle.poses[i].applyLeftIncrement(d);
  }

  // Run one iteration at a time and assert the cost never increases: a step
  // that makes things worse must be rejected by the LM damping, not accepted.
  double previous = std::numeric_limits<double>::max();
  for (int i = 0; i < 12; ++i) {
    BundleOptions options;
    options.max_iterations = 1;
    const BundleSummary s = optimizeBundle(&p.bundle, options);
    EXPECT_LE(s.final_cost, previous + 1e-9) << "iteration " << i;
    previous = s.final_cost;
  }
}

TEST(Bundle, ConvergesUnderObservationNoise) {
  Problem p = makeProblem(8, 130, 77, /*noise_px=*/0.5);
  const double rmse_before = bundleRmsePixels(p.bundle);

  BundleOptions options;
  options.max_iterations = 60;
  const BundleSummary summary = optimizeBundle(&p.bundle, options);
  EXPECT_TRUE(summary.success);
  // With 0.5 px isotropic noise the optimum sits near 0.5 px, not at zero.
  EXPECT_LT(summary.final_rmse_px, rmse_before);
  EXPECT_LT(summary.final_rmse_px, 0.75);
  // Structure should still land close to the truth.
  EXPECT_LT(cloudError(p.bundle.points, p.true_points), 0.02);
}

TEST(Bundle, RespectsConstantPosesAndPoints) {
  Problem p = makeProblem(5, 50, 5);
  const std::vector<Pose> poses_before = p.bundle.poses;
  const std::vector<Vec3> points_before = p.bundle.points;

  p.bundle.constant_poses.assign(p.bundle.poses.size(), 1);
  p.bundle.constant_points.assign(p.bundle.points.size(), 1);

  const BundleSummary summary = optimizeBundle(&p.bundle);
  EXPECT_TRUE(summary.success);
  for (size_t i = 0; i < poses_before.size(); ++i) {
    EXPECT_LT((p.bundle.poses[i].t - poses_before[i].t).norm(), 1e-15);
    EXPECT_LT((p.bundle.poses[i].R - poses_before[i].R).norm(), 1e-15);
  }
  for (size_t i = 0; i < points_before.size(); ++i) {
    EXPECT_LT((p.bundle.points[i] - points_before[i]).norm(), 1e-15);
  }
}

TEST(Bundle, FirstFreePoseIsHeldByTheGauge) {
  Problem p = makeProblem(6, 60, 8);
  const Pose first_before = p.bundle.poses[0];
  Rng rng(9);
  for (Vec3& x : p.bundle.points) x += Vec3(rng.normal(), rng.normal(), rng.normal()) * 0.03;

  BundleOptions options;
  options.fix_first_pose = true;
  optimizeBundle(&p.bundle, options);
  EXPECT_LT((p.bundle.poses[0].t - first_before.t).norm(), 1e-14);
  EXPECT_LT((p.bundle.poses[0].R - first_before.R).norm(), 1e-14);
}

TEST(Bundle, ScaleGaugePreservesTheBaseline) {
  Problem p = makeProblem(6, 60, 12);

  Rng rng(13);
  for (size_t i = 1; i < p.bundle.poses.size(); ++i) {
    Eigen::Matrix<double, 6, 1> d;
    for (int c = 0; c < 6; ++c) d(c) = rng.normal() * 0.02;
    p.bundle.poses[i].applyLeftIncrement(d);
  }
  for (Vec3& x : p.bundle.points) x += Vec3(rng.normal(), rng.normal(), rng.normal()) * 0.02;

  // The gauge pins whatever baseline the solver *starts* from, so it has to be
  // measured after the perturbation, not before it.
  const double baseline_before =
      (p.bundle.poses[0].center() - p.bundle.poses[1].center()).norm();

  BundleOptions options;
  options.fix_scale = true;
  options.max_iterations = 50;
  const BundleSummary summary = optimizeBundle(&p.bundle, options);
  ASSERT_TRUE(summary.success);

  const double baseline_after =
      (p.bundle.poses[0].center() - p.bundle.poses[1].center()).norm();
  EXPECT_NEAR(baseline_after, baseline_before, 1e-9 * baseline_before);
}

TEST(Bundle, RefusesProblemsTooLargeForTheDenseSolver) {
  Problem p = makeProblem(5, 30);
  BundleOptions options;
  options.max_dense_cameras = 2;
  const BundleSummary summary = optimizeBundle(&p.bundle, options);
  EXPECT_FALSE(summary.success);
  EXPECT_NE(summary.message.find("too many free cameras"), std::string::npos);
}

TEST(Bundle, PruneRemovesOutliersAndOrphanedPoints) {
  Problem p = makeProblem(5, 40, 21);
  const size_t before = p.bundle.observations.size();

  // Wreck a handful of observations.
  for (size_t i = 0; i < 20 && i < p.bundle.observations.size(); ++i) {
    p.bundle.observations[i].pixel += Vec2(150.0, 120.0);
  }
  const int removed = pruneOutlierObservations(&p.bundle, 3.0);
  EXPECT_GE(removed, 20);
  EXPECT_LT(p.bundle.observations.size(), before);

  // Every surviving point must retain at least two rays.
  std::vector<int> counts(p.bundle.points.size(), 0);
  for (const Observation& o : p.bundle.observations) ++counts[o.point];
  for (const Observation& o : p.bundle.observations) EXPECT_GE(counts[o.point], 2);

  EXPECT_EQ(pruneOutlierObservations(nullptr, 1.0), 0);
}

TEST(Bundle, HuberLossLimitsTheDamageFromGrossOutliers) {
  Problem clean = makeProblem(7, 100, 41);
  Problem dirty = makeProblem(7, 100, 41);
  // 5% of observations get a large error, left in place (not pruned).
  for (size_t i = 0; i < dirty.bundle.observations.size() / 20; ++i) {
    dirty.bundle.observations[i].pixel += Vec2(120.0, -95.0);
  }

  Rng rng(42);
  for (size_t i = 1; i < clean.bundle.poses.size(); ++i) {
    Eigen::Matrix<double, 6, 1> d;
    for (int c = 0; c < 6; ++c) d(c) = rng.normal() * 0.015;
    clean.bundle.poses[i].applyLeftIncrement(d);
    dirty.bundle.poses[i].applyLeftIncrement(d);
  }

  BundleOptions options;
  options.max_iterations = 60;
  options.huber_delta_px = 2.0;
  optimizeBundle(&clean.bundle, options);
  optimizeBundle(&dirty.bundle, options);

  const double clean_error = cloudError(clean.bundle.points, clean.true_points);
  const double dirty_error = cloudError(dirty.bundle.points, dirty.true_points);
  // The outliers must not blow the structure up; a plain L2 loss would.
  EXPECT_LT(dirty_error, 0.05) << "clean " << clean_error << " dirty " << dirty_error;
}
