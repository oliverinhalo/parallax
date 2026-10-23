#include "parallax/sfm/TwoView.h"

#include <gtest/gtest.h>

#include "parallax/sfm/Triangulation.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

struct Rig {
  CameraIntrinsics k;
  Pose pose_a;
  Pose pose_b;
  std::vector<Vec3> points;
  std::vector<PointPair> pairs;
};

/// Build a well-conditioned two-view problem: a cloud of 3D points filling the
/// frustum, observed from two cameras with a healthy baseline.
Rig makeRig(int point_count = 120, double baseline = 0.35, uint64_t seed = 42,
            double noise_px = 0.0, bool planar = false) {
  Rig rig;
  rig.k = phoneIntrinsics(640, 480);
  rig.pose_a = SyntheticScene::lookAt(Vec3(0, 0, 0), Vec3(0, 0, 1));
  rig.pose_b = SyntheticScene::lookAt(Vec3(baseline, 0.05, 0.0), Vec3(0, 0, 1));

  Rng rng(seed);
  while (static_cast<int>(rig.points.size()) < point_count) {
    const Vec3 p = planar
                       ? Vec3(rng.uniform(-1.2, 1.2), rng.uniform(-0.9, 0.9), 3.0)
                       : Vec3(rng.uniform(-1.2, 1.2), rng.uniform(-0.9, 0.9),
                              rng.uniform(2.0, 6.0));
    Vec2 pa, pb;
    if (!rig.k.project(rig.pose_a * p, &pa)) continue;
    if (!rig.k.project(rig.pose_b * p, &pb)) continue;
    const auto inside = [&rig](const Vec2& px) {
      return px.x() > 2 && px.y() > 2 && px.x() < rig.k.width - 3 && px.y() < rig.k.height - 3;
    };
    if (!inside(pa) || !inside(pb)) continue;
    if (noise_px > 0.0) {
      pa += Vec2(rng.normal(), rng.normal()) * noise_px;
      pb += Vec2(rng.normal(), rng.normal()) * noise_px;
    }
    rig.points.push_back(p);
    rig.pairs.push_back(PointPair{pa, pb});
  }
  return rig;
}

/// Ground-truth relative pose: b-from-a.
Pose relativePose(const Pose& a, const Pose& b) { return b * a.inverse(); }

}  // namespace

TEST(Normalization, ProducesZeroMeanAndRootTwoScale) {
  std::vector<Vec2> pts;
  Rng rng(3);
  for (int i = 0; i < 200; ++i) pts.emplace_back(rng.uniform(100, 900), rng.uniform(50, 700));
  const Mat3 T = normalizingTransform(pts);

  Vec2 mean = Vec2::Zero();
  double mean_dist = 0.0;
  std::vector<Vec2> transformed;
  for (const Vec2& p : pts) {
    const Vec3 q = T * Vec3(p.x(), p.y(), 1.0);
    transformed.emplace_back(q.x(), q.y());
    mean += transformed.back();
  }
  mean /= static_cast<double>(pts.size());
  for (const Vec2& q : transformed) mean_dist += (q - mean).norm();
  mean_dist /= static_cast<double>(pts.size());

  EXPECT_LT(mean.norm(), 1e-9);
  EXPECT_NEAR(mean_dist, std::sqrt(2.0), 1e-9);
}

TEST(Normalization, HandlesEmptyAndDegenerateInput) {
  EXPECT_TRUE(normalizingTransform({}).isApprox(Mat3::Identity()));
  // All points coincident: scale is undefined, must not produce NaN.
  const Mat3 T = normalizingTransform({Vec2(5, 5), Vec2(5, 5), Vec2(5, 5)});
  EXPECT_TRUE(T.allFinite());
}

TEST(Fundamental, SatisfiesEpipolarConstraintOnNoiseFreeData) {
  const Rig rig = makeRig();
  Mat3 F;
  ASSERT_TRUE(fundamentalFromPairs(rig.pairs, &F));
  for (const PointPair& p : rig.pairs) {
    EXPECT_LT(sampsonDistance(F, p), 1e-6);
  }
}

TEST(Fundamental, IsRankTwo) {
  const Rig rig = makeRig();
  Mat3 F;
  ASSERT_TRUE(fundamentalFromPairs(rig.pairs, &F));
  Eigen::JacobiSVD<Mat3> svd(F);
  const Vec3 s = svd.singularValues();
  EXPECT_LT(s(2) / s(0), 1e-10) << "smallest singular value must be zero";
}

TEST(Fundamental, RejectsTooFewCorrespondences) {
  const Rig rig = makeRig(7);
  Mat3 F;
  std::vector<PointPair> few(rig.pairs.begin(), rig.pairs.begin() + 7);
  EXPECT_FALSE(fundamentalFromPairs(few, &F));
}

TEST(Essential, ProjectionEnforcesEqualSingularValues) {
  Rng rng(9);
  Mat3 M;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) M(i, j) = rng.normal();
  }
  const Mat3 E = projectToEssential(M);
  Eigen::JacobiSVD<Mat3> svd(E);
  const Vec3 s = svd.singularValues();
  EXPECT_NEAR(s(0), s(1), 1e-12);
  EXPECT_NEAR(s(2), 0.0, 1e-12);
}

TEST(Essential, DecompositionYieldsFourValidRotations) {
  const Rig rig = makeRig();
  Mat3 F;
  ASSERT_TRUE(fundamentalFromPairs(rig.pairs, &F));
  const Mat3 E = projectToEssential(essentialFromFundamental(F, rig.k, rig.k));

  std::array<Pose, 4> candidates;
  decomposeEssential(E, &candidates);
  for (const Pose& p : candidates) {
    EXPECT_NEAR(p.R.determinant(), 1.0, 1e-9);
    EXPECT_TRUE((p.R * p.R.transpose()).isApprox(Mat3::Identity(), 1e-9));
    EXPECT_NEAR(p.t.norm(), 1.0, 1e-9);
  }
  // The four candidates are two rotations, each with both translation signs.
  EXPECT_LT((candidates[0].t + candidates[1].t).norm(), 1e-12);
  EXPECT_LT((candidates[2].t + candidates[3].t).norm(), 1e-12);
}

TEST(Essential, RecoversTheGroundTruthRelativePose) {
  const Rig rig = makeRig(150, 0.4);
  const TwoViewGeometry geo = estimateTwoView(rig.pairs, rig.k, rig.k);
  ASSERT_TRUE(geo.success);
  EXPECT_GT(geo.inlier_count, 140);

  const Pose expected = relativePose(rig.pose_a, rig.pose_b);
  // Rotation is fully determined.
  EXPECT_LT(rad2deg(Pose::rotationAngle(geo.relative_pose, expected)), 0.1);
  // Translation is only determined up to scale, so compare directions.
  const double cos_angle =
      geo.relative_pose.t.normalized().dot(expected.t.normalized());
  EXPECT_GT(cos_angle, 0.9999) << "translation direction off by "
                               << rad2deg(std::acos(std::min(1.0, cos_angle))) << " deg";
}

TEST(Essential, ToleratesPixelNoise) {
  const Rig rig = makeRig(220, 0.45, 1234, 0.5);
  const TwoViewGeometry geo = estimateTwoView(rig.pairs, rig.k, rig.k);
  ASSERT_TRUE(geo.success);
  const Pose expected = relativePose(rig.pose_a, rig.pose_b);
  EXPECT_LT(rad2deg(Pose::rotationAngle(geo.relative_pose, expected)), 1.5);
  const double cos_angle = geo.relative_pose.t.normalized().dot(expected.t.normalized());
  EXPECT_GT(cos_angle, 0.995);
  EXPECT_LT(geo.median_sampson, 2.0);
}

TEST(Essential, SurvivesOutliers) {
  Rig rig = makeRig(200, 0.4, 7, 0.3);
  // Corrupt 30% of the correspondences with random mismatches.
  Rng rng(555);
  const size_t corrupt = rig.pairs.size() * 3 / 10;
  for (size_t i = 0; i < corrupt; ++i) {
    rig.pairs[i].b = Vec2(rng.uniform(0, rig.k.width), rng.uniform(0, rig.k.height));
  }
  const TwoViewGeometry geo = estimateTwoView(rig.pairs, rig.k, rig.k);
  ASSERT_TRUE(geo.success);

  const Pose expected = relativePose(rig.pose_a, rig.pose_b);
  EXPECT_LT(rad2deg(Pose::rotationAngle(geo.relative_pose, expected)), 2.0);
  // The outliers must be identified, not absorbed.
  EXPECT_LT(geo.inlier_count, static_cast<int>(rig.pairs.size()));
  EXPECT_GT(geo.inlier_count, static_cast<int>(rig.pairs.size() * 0.6));
  int outliers_rejected = 0;
  for (size_t i = 0; i < corrupt; ++i) {
    if (!geo.inliers[i]) ++outliers_rejected;
  }
  EXPECT_GT(static_cast<double>(outliers_rejected) / corrupt, 0.85);
}

TEST(Essential, FlagsPlanarScenesAsDegenerate) {
  // A planar scene is the classic degenerate configuration: infinitely many
  // essential matrices explain it, so initialisation must refuse the pair.
  const Rig planar = makeRig(150, 0.4, 11, 0.0, /*planar=*/true);
  const TwoViewGeometry geo = estimateTwoView(planar.pairs, planar.k, planar.k);
  EXPECT_GT(geo.planarity, 0.9) << "planar scene should be fully explained by a homography";

  const Rig general = makeRig(150, 0.4, 11, 0.0, /*planar=*/false);
  const TwoViewGeometry geo2 = estimateTwoView(general.pairs, general.k, general.k);
  EXPECT_LT(geo2.planarity, 0.8) << "a 3D scene should not be homography-explainable";
}

TEST(Essential, FailsGracefullyOnDegenerateInput) {
  EXPECT_FALSE(estimateTwoView({}, phoneIntrinsics(), phoneIntrinsics()).success);
  std::vector<PointPair> identical(30, PointPair{Vec2(100, 100), Vec2(100, 100)});
  const TwoViewGeometry geo = estimateTwoView(identical, phoneIntrinsics(), phoneIntrinsics());
  EXPECT_FALSE(geo.success);
}

TEST(Homography, RecoversAPlanarMapping) {
  // Synthesise a homography and check we invert the construction exactly.
  Mat3 H;
  H << 1.08, 0.05, 12.0,
       -0.04, 0.97, -7.0,
       1e-4, 2e-5, 1.0;
  Rng rng(21);
  std::vector<PointPair> pairs;
  for (int i = 0; i < 40; ++i) {
    const Vec2 a(rng.uniform(50, 600), rng.uniform(50, 420));
    const Vec3 b = H * Vec3(a.x(), a.y(), 1.0);
    pairs.push_back(PointPair{a, b.head<2>() / b.z()});
  }
  Mat3 estimated;
  ASSERT_TRUE(homographyFromPairs(pairs, &estimated));
  for (const PointPair& p : pairs) {
    EXPECT_LT(homographyTransferError(estimated, p), 1e-6);
  }
}

TEST(Homography, RejectsTooFewPoints) {
  Mat3 H;
  EXPECT_FALSE(homographyFromPairs({PointPair{}, PointPair{}, PointPair{}}, &H));
}

// ----------------------------------------------------------- triangulation ---

TEST(Triangulation, RecoversPointsExactlyWithoutNoise) {
  const Rig rig = makeRig(80, 0.5);
  for (size_t i = 0; i < rig.pairs.size(); ++i) {
    const TriangulationResult tri = triangulateDlt(rig.pose_a, rig.k, rig.pairs[i].a,
                                                   rig.pose_b, rig.k, rig.pairs[i].b);
    ASSERT_TRUE(tri.valid) << "point " << i;
    EXPECT_LT((tri.point - rig.points[i]).norm(), 1e-9);
  }
}

TEST(Triangulation, RejectsLowParallax) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose a = SyntheticScene::lookAt(Vec3(0, 0, 0), Vec3(0, 0, 1));
  // A 1 mm baseline looking at a point 5 m away: parallax is ~0.01 degrees.
  const Pose b = SyntheticScene::lookAt(Vec3(0.001, 0, 0), Vec3(0, 0, 1));
  const Vec3 p(0.1, 0.05, 5.0);
  Vec2 pa, pb;
  ASSERT_TRUE(k.project(a * p, &pa));
  ASSERT_TRUE(k.project(b * p, &pb));

  EXPECT_FALSE(triangulateDlt(a, k, pa, b, k, pb, 1.0).valid) << "should reject low parallax";
  // With the gate removed the geometry still solves.
  EXPECT_TRUE(triangulateDlt(a, k, pa, b, k, pb, 0.0).valid);
}

TEST(Triangulation, RejectsPointsBehindTheCamera) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Pose a = SyntheticScene::lookAt(Vec3(0, 0, 0), Vec3(0, 0, 1));
  const Pose b = SyntheticScene::lookAt(Vec3(0.4, 0, 0), Vec3(0, 0, 1));
  // Mirror the observation so the rays diverge behind the cameras.
  const TriangulationResult tri =
      triangulateDlt(a, k, Vec2(500, 240), b, k, Vec2(100, 240));
  EXPECT_FALSE(tri.valid);
}

TEST(Triangulation, MultiViewBeatsTwoViewUnderNoise) {
  const CameraIntrinsics k = phoneIntrinsics(640, 480);
  const Vec3 truth(0.12, -0.08, 3.1);
  const std::vector<Pose> poses =
      SyntheticScene::orbitPoses(truth, 1.4, 0.0, 8, 0.0, deg2rad(90.0));

  Rng rng(99);
  std::vector<Observation> observations;
  std::vector<CameraIntrinsics> intrinsics(poses.size(), k);
  for (size_t i = 0; i < poses.size(); ++i) {
    Vec2 px;
    ASSERT_TRUE(k.project(poses[i] * truth, &px));
    px += Vec2(rng.normal(), rng.normal()) * 0.8;  // sub-pixel noise
    observations.push_back(Observation{static_cast<int>(i), 0, px, 1.0f});
  }

  const TriangulationResult two =
      triangulateMultiView(poses, intrinsics, {observations[0], observations[1]}, 0.5, 1e9);
  const TriangulationResult many =
      triangulateMultiView(poses, intrinsics, observations, 0.5, 1e9);
  ASSERT_TRUE(two.valid);
  ASSERT_TRUE(many.valid);
  // Averaging over eight rays must beat two of them.
  EXPECT_LT((many.point - truth).norm(), (two.point - truth).norm());
  EXPECT_LT((many.point - truth).norm(), 0.01);
}

TEST(Triangulation, HandlesDegenerateObservationLists) {
  const std::vector<Pose> poses{Pose::Identity()};
  const std::vector<CameraIntrinsics> k{phoneIntrinsics()};
  EXPECT_FALSE(triangulateMultiView(poses, k, {}, 1.0).valid);
  EXPECT_FALSE(triangulateMultiView(poses, k, {Observation{0, 0, Vec2(1, 1), 1.f}}, 1.0).valid);
  // Out-of-range camera index must be rejected rather than read out of bounds.
  EXPECT_FALSE(triangulateMultiView(
                   poses, k,
                   {Observation{0, 0, Vec2(1, 1), 1.f}, Observation{5, 0, Vec2(2, 2), 1.f}}, 1.0)
                   .valid);
}

TEST(Parallax, IsZeroForCoincidentCentresAndNinetyForRightAngles) {
  EXPECT_NEAR(parallaxDegrees(Vec3(0, 0, 0), Vec3(0, 0, 0), Vec3(0, 0, 1)), 0.0, 1e-9);
  EXPECT_NEAR(parallaxDegrees(Vec3(-1, 0, 0), Vec3(0, 0, -1), Vec3(0, 0, 0)), 90.0, 1e-9);
}

// ------------------------------------------------------------------ ransac ---

TEST(Ransac, FindsTheDominantModelAmongOutliers) {
  // Fit a scalar offset to 1D data: 70% clustered at 5.0, 30% uniform noise.
  Rng rng(17);
  std::vector<double> data;
  for (int i = 0; i < 70; ++i) data.push_back(5.0 + rng.normal() * 0.05);
  for (int i = 0; i < 30; ++i) data.push_back(rng.uniform(-50, 50));

  RansacOptions options;
  options.inlier_threshold = 0.3;
  const auto fit = [&data](const std::vector<int>& sample, std::vector<double>* out) {
    out->push_back(data[sample[0]]);
    return true;
  };
  const auto residual = [&data](const double& model, int i) {
    return std::abs(data[i] - model);
  };
  const RansacResult<double> r =
      runRansac<double>(static_cast<int>(data.size()), 1, options, fit, residual);
  ASSERT_TRUE(r.success);
  EXPECT_NEAR(r.model, 5.0, 0.2);
  EXPECT_GE(r.inlier_count, 65);
}

TEST(Ransac, StopsEarlyWhenEverythingIsAnInlier) {
  std::vector<double> data(50, 2.0);
  RansacOptions options;
  options.inlier_threshold = 1e-6;
  options.max_iterations = 5000;
  options.min_iterations = 1;
  const auto fit = [&data](const std::vector<int>& s, std::vector<double>* out) {
    out->push_back(data[s[0]]);
    return true;
  };
  const auto residual = [&data](const double& m, int i) { return std::abs(data[i] - m); };
  const RansacResult<double> r = runRansac<double>(50, 1, options, fit, residual);
  EXPECT_TRUE(r.success);
  EXPECT_LT(r.iterations, 20) << "adaptive stopping should kick in immediately";
}

TEST(Ransac, ReturnsFailureWhenTooFewData) {
  const auto fit = [](const std::vector<int>&, std::vector<double>* out) {
    out->push_back(0.0);
    return true;
  };
  const auto residual = [](const double&, int) { return 0.0; };
  EXPECT_FALSE(runRansac<double>(3, 8, {}, fit, residual).success);
}

TEST(Ransac, SampleDistinctProducesUniqueIndices) {
  Rng rng(5);
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<int> sample;
    ASSERT_TRUE(sampleDistinct(rng, 20, 8, &sample));
    EXPECT_EQ(sample.size(), 8u);
    std::sort(sample.begin(), sample.end());
    EXPECT_EQ(std::unique(sample.begin(), sample.end()), sample.end());
    for (int i : sample) {
      EXPECT_GE(i, 0);
      EXPECT_LT(i, 20);
    }
  }
  std::vector<int> impossible;
  EXPECT_FALSE(sampleDistinct(rng, 3, 8, &impossible));
}
