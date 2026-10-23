#include "parallax/geometry/SE3.h"

#include <gtest/gtest.h>

#include <random>

using namespace parallax;

namespace {

std::mt19937& rng() {
  static std::mt19937 gen(1234);
  return gen;
}

Vec3 randomVec(double scale) {
  std::uniform_real_distribution<double> d(-scale, scale);
  return Vec3(d(rng()), d(rng()), d(rng()));
}

Pose randomPose() {
  return Pose(so3Exp(randomVec(2.5)), randomVec(3.0));
}

}  // namespace

TEST(SO3, ExpOfZeroIsIdentity) {
  EXPECT_TRUE(so3Exp(Vec3::Zero()).isApprox(Mat3::Identity(), 1e-15));
}

TEST(SO3, ExpProducesValidRotation) {
  for (int i = 0; i < 200; ++i) {
    const Mat3 R = so3Exp(randomVec(3.0));
    EXPECT_NEAR(R.determinant(), 1.0, 1e-12);
    EXPECT_TRUE((R * R.transpose()).isApprox(Mat3::Identity(), 1e-12));
  }
}

TEST(SO3, LogIsInverseOfExp) {
  for (int i = 0; i < 500; ++i) {
    Vec3 w = randomVec(1.0);
    // Keep the angle inside (0, pi) where the log is unique.
    if (w.norm() < 1e-6) continue;
    w = w.normalized() * std::fmod(w.norm(), kPi - 1e-3);
    const Vec3 recovered = so3Log(so3Exp(w));
    EXPECT_LT((recovered - w).norm(), 1e-9) << "w = " << w.transpose();
  }
}

TEST(SO3, LogHandlesSmallAngles) {
  for (double angle : {1e-10, 1e-8, 1e-6, 1e-4}) {
    const Vec3 w = Vec3(1, 2, -3).normalized() * angle;
    const Vec3 recovered = so3Log(so3Exp(w));
    EXPECT_LT((recovered - w).norm(), 1e-12 + angle * 1e-6) << "angle " << angle;
  }
}

TEST(SO3, LogHandlesAnglesNearPi) {
  // This is the branch the naive formula gets wrong: sin(theta) -> 0.
  for (double eps : {1e-7, 1e-6, 1e-4, 1e-2}) {
    const Vec3 axis = Vec3(0.3, -0.7, 0.64).normalized();
    const Vec3 w = axis * (kPi - eps);
    const Mat3 R = so3Exp(w);
    const Vec3 recovered = so3Log(R);
    // exp(log(R)) must reproduce R even if the sign of the axis flips.
    EXPECT_TRUE(so3Exp(recovered).isApprox(R, 1e-7)) << "eps " << eps;
    EXPECT_NEAR(recovered.norm(), kPi - eps, 1e-5) << "eps " << eps;
  }
}

TEST(SO3, SkewMatchesCrossProduct) {
  for (int i = 0; i < 50; ++i) {
    const Vec3 a = randomVec(2.0), b = randomVec(2.0);
    EXPECT_LT((skew(a) * b - a.cross(b)).norm(), 1e-14);
  }
}

TEST(SE3, ExpLogRoundTrip) {
  for (int i = 0; i < 300; ++i) {
    Eigen::Matrix<double, 6, 1> xi;
    xi.head<3>() = randomVec(2.0);
    Vec3 w = randomVec(1.0);
    if (w.norm() > 1e-6) w = w.normalized() * std::fmod(w.norm(), kPi - 1e-3);
    xi.tail<3>() = w;

    const Eigen::Matrix<double, 6, 1> recovered = Pose::exp(xi).log();
    EXPECT_LT((recovered - xi).norm(), 1e-9) << "xi = " << xi.transpose();
  }
}

TEST(SE3, InverseUndoesTransform) {
  for (int i = 0; i < 100; ++i) {
    const Pose p = randomPose();
    const Vec3 x = randomVec(5.0);
    EXPECT_LT((p.inverse() * (p * x) - x).norm(), 1e-12);
  }
}

TEST(SE3, CompositionIsAssociativeAndMatchesPointTransform) {
  for (int i = 0; i < 100; ++i) {
    const Pose a = randomPose(), b = randomPose(), c = randomPose();
    const Vec3 x = randomVec(4.0);
    EXPECT_LT((((a * b) * c) * x - (a * (b * c)) * x).norm(), 1e-11);
    EXPECT_LT(((a * b) * x - a * (b * x)).norm(), 1e-12);
  }
}

TEST(SE3, CenterIsThePointThatMapsToOrigin) {
  for (int i = 0; i < 100; ++i) {
    const Pose p = randomPose();
    EXPECT_LT((p * p.center()).norm(), 1e-11);
  }
}

TEST(SE3, WorldFromCameraRoundTrip) {
  for (int i = 0; i < 100; ++i) {
    const Pose p = randomPose();
    const Pose back = Pose::fromWorldFromCamera(p.worldFromCamera());
    EXPECT_LT((back.R - p.R).norm(), 1e-11);
    EXPECT_LT((back.t - p.t).norm(), 1e-11);
  }
}

TEST(SE3, InterpolationHitsEndpointsAndStaysOnManifold) {
  const Pose a = randomPose(), b = randomPose();
  EXPECT_LT((Pose::interpolate(a, b, 0.0).t - a.t).norm(), 1e-11);
  EXPECT_LT((Pose::interpolate(a, b, 1.0).t - b.t).norm(), 1e-10);
  for (double alpha : {0.1, 0.25, 0.5, 0.75, 0.9}) {
    const Pose m = Pose::interpolate(a, b, alpha);
    EXPECT_NEAR(m.R.determinant(), 1.0, 1e-11);
  }
}

TEST(SE3, RotationAngleIsSymmetricAndZeroForEqualPoses) {
  const Pose a = randomPose(), b = randomPose();
  EXPECT_NEAR(Pose::rotationAngle(a, a), 0.0, 1e-12);
  EXPECT_NEAR(Pose::rotationAngle(a, b), Pose::rotationAngle(b, a), 1e-11);
}

TEST(SE3, LeftIncrementMatchesExplicitComposition) {
  Pose p = randomPose();
  Eigen::Matrix<double, 6, 1> d;
  d << 0.01, -0.02, 0.03, 0.004, -0.005, 0.006;
  const Pose expected = Pose::exp(d) * p;
  p.applyLeftIncrement(d);
  EXPECT_LT((p.R - expected.R).norm(), 1e-11);
  EXPECT_LT((p.t - expected.t).norm(), 1e-11);
}

TEST(Orthonormalize, RepairsDriftedRotation) {
  Mat3 R = so3Exp(Vec3(0.3, -0.2, 0.8));
  R += Mat3::Constant(1e-3);  // simulate accumulated float drift
  const Mat3 fixed = orthonormalize(R);
  EXPECT_NEAR(fixed.determinant(), 1.0, 1e-13);
  EXPECT_TRUE((fixed * fixed.transpose()).isApprox(Mat3::Identity(), 1e-13));
}

TEST(AverageRotations, ReturnsTheCommonRotationAndHandlesEmpty) {
  EXPECT_TRUE(averageRotations({}).isApprox(Mat3::Identity()));
  const Mat3 R = so3Exp(Vec3(0.1, 0.2, -0.3));
  EXPECT_TRUE(averageRotations({R, R, R}).isApprox(R, 1e-12));

  // A symmetric spread about R must average back to R.
  const Mat3 d = so3Exp(Vec3(0.05, 0, 0));
  const Mat3 mean = averageRotations({R * d, R, R * d.transpose()});
  EXPECT_LT(so3Log(mean * R.transpose()).norm(), 1e-9);
}

TEST(Intrinsics, ProjectUnprojectRoundTrip) {
  CameraIntrinsics k;
  k.fx = 1200; k.fy = 1205; k.cx = 640; k.cy = 360; k.width = 1280; k.height = 720;
  for (int i = 0; i < 200; ++i) {
    const Vec3 p(randomVec(1.0).x(), randomVec(1.0).y(), 2.0 + std::abs(randomVec(3.0).z()));
    Vec2 px;
    ASSERT_TRUE(k.project(p, &px));
    const Vec3 ray = k.unproject(px);
    // The ray must be parallel to p.
    EXPECT_LT((ray.normalized() - p.normalized()).norm(), 1e-12);
  }
}

TEST(Intrinsics, ProjectUnprojectRoundTripWithDistortion) {
  CameraIntrinsics k;
  k.fx = 900; k.fy = 900; k.cx = 320; k.cy = 240; k.width = 640; k.height = 480;
  k.k1 = -0.18; k.k2 = 0.04;
  for (int i = 0; i < 200; ++i) {
    const Vec3 p(randomVec(0.6).x(), randomVec(0.6).y(), 2.0 + std::abs(randomVec(2.0).z()));
    Vec2 px;
    ASSERT_TRUE(k.project(p, &px));
    const Vec3 ray = k.unproject(px);
    EXPECT_LT((ray.normalized() - p.normalized()).norm(), 1e-6);
  }
}

TEST(Intrinsics, RejectsPointsBehindCamera) {
  CameraIntrinsics k;
  k.fx = 500; k.fy = 500; k.cx = 160; k.cy = 120; k.width = 320; k.height = 240;
  Vec2 px;
  EXPECT_FALSE(k.project(Vec3(0, 0, -1), &px));
  EXPECT_FALSE(k.project(Vec3(0, 0, 0), &px));
}
