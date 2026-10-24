#include "parallax/depth/MultiViewStereo.h"

#include "parallax/testing/SyntheticScene.h"

#include <gtest/gtest.h>

#include <cmath>

namespace parallax {
namespace {

using testing::SyntheticScene;

struct RenderedView {
  GrayBuffer gray;
  DepthMap truth;
  Pose pose;
  CameraIntrinsics intrinsics;
};

/// Render an orbit of a scene, keeping each view's image, pose and true depth.
std::vector<RenderedView> renderOrbit(const SyntheticScene& scene, const Vec3& target,
                                      double radius, double elevation, int count,
                                      int width = 160, int height = 120) {
  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(width, height);
  const std::vector<Pose> poses =
      SyntheticScene::orbitPoses(target, radius, elevation, count);
  std::vector<RenderedView> views;
  views.reserve(poses.size());
  for (const Pose& pose : poses) {
    const auto rendered = scene.render(intrinsics, pose, /*want_color=*/false,
                                       /*want_depth=*/true);
    RenderedView view;
    view.gray = rendered.gray;
    view.truth = rendered.depth;
    view.pose = pose;
    view.intrinsics = intrinsics;
    views.push_back(std::move(view));
  }
  return views;
}

std::vector<MvsView> asMvsViews(const std::vector<RenderedView>& rendered) {
  std::vector<MvsView> views;
  views.reserve(rendered.size());
  for (const RenderedView& view : rendered) {
    MvsView entry;
    entry.gray = &view.gray;
    entry.pose = view.pose;
    entry.intrinsics = view.intrinsics;
    views.push_back(entry);
  }
  return views;
}

struct Accuracy {
  int measured = 0;
  int total = 0;
  double mean_absolute_error = 0.0;
  double median_absolute_error = 0.0;
  double inlier_fraction = 0.0;  ///< within 5% of the truth
};

/**
 * Compare an estimate against ground truth.
 *
 * The estimate is at a lower resolution than the truth (the sweep downsamples),
 * so each estimated pixel is compared against the truth at the matching
 * position rather than the two being differenced directly.
 */
Accuracy compare(const DepthMap& estimate, const DepthMap& truth) {
  Accuracy accuracy;
  if (estimate.empty() || truth.empty()) return accuracy;
  const double scale_x = static_cast<double>(truth.width) / estimate.width;
  const double scale_y = static_cast<double>(truth.height) / estimate.height;

  std::vector<double> errors;
  double total_error = 0.0;
  int inliers = 0;
  for (int y = 0; y < estimate.height; ++y) {
    for (int x = 0; x < estimate.width; ++x) {
      const int tx = std::min(truth.width - 1, static_cast<int>((x + 0.5) * scale_x));
      const int ty = std::min(truth.height - 1, static_cast<int>((y + 0.5) * scale_y));
      const float expected = truth.at(tx, ty);
      if (expected <= 0.f) continue;  // nothing there in the ground truth
      ++accuracy.total;
      const float measured = estimate.at(x, y);
      if (measured <= 0.f) continue;
      ++accuracy.measured;
      const double error = std::abs(measured - expected);
      errors.push_back(error);
      total_error += error;
      if (error <= expected * 0.05) ++inliers;
    }
  }
  if (!errors.empty()) {
    accuracy.mean_absolute_error = total_error / errors.size();
    std::sort(errors.begin(), errors.end());
    accuracy.median_absolute_error = errors[errors.size() / 2];
    accuracy.inlier_fraction = static_cast<double>(inliers) / errors.size();
  }
  return accuracy;
}

/**
 * The headline test: depth from images and poses alone, on a textured scene.
 *
 * This is the path a device with no depth sensor and no depth model takes, so
 * the numbers here are what reconstruction quality rests on for those devices.
 */
TEST(MvsTest, RecoversDepthOnATexturedObject) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.1, 0.35, 12);
  const auto views = asMvsViews(rendered);

  const int reference = 6;
  const std::vector<int> neighbour_indices = selectNeighbours(views, reference, 4, 1.1);
  ASSERT_FALSE(neighbour_indices.empty());
  std::vector<MvsView> neighbours;
  for (int index : neighbour_indices) neighbours.push_back(views[index]);

  MvsConfig config;
  config.min_depth = 0.5;
  config.max_depth = 2.0;
  config.depth_samples = 64;

  const DepthMap estimate = estimateDepth(views[reference], neighbours, config);
  ASSERT_FALSE(estimate.empty());

  const Accuracy accuracy = compare(estimate, rendered[reference].truth);
  // Coverage: at least a third of the pixels that have a true depth get one.
  // The rest are the untextured table top and the sky, where there is nothing
  // to correlate and inventing a depth would be worse than leaving it out.
  EXPECT_GT(accuracy.measured, accuracy.total / 3)
      << accuracy.measured << " of " << accuracy.total;
  // Accuracy: the median error is a couple of centimetres at about a metre.
  EXPECT_LT(accuracy.median_absolute_error, 0.06)
      << "median " << accuracy.median_absolute_error;
  EXPECT_GT(accuracy.inlier_fraction, 0.5) << "inliers " << accuracy.inlier_fraction;
}

TEST(MvsTest, RecoversDepthInsideARoom) {
  const SyntheticScene scene = SyntheticScene::room();
  // Looking at a corner from two positions a little apart, which is what a
  // room scan's keyframes actually look like.
  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(160, 120);
  std::vector<RenderedView> rendered;
  for (double offset : {-0.12, 0.0, 0.12, 0.24}) {
    const Pose pose = SyntheticScene::lookAt(Vec3(offset, 1.3, 0.4), Vec3(1.6, 1.1, 1.2));
    const auto result = scene.render(intrinsics, pose, false, true);
    rendered.push_back({result.gray, result.depth, pose, intrinsics});
  }
  const auto views = asMvsViews(rendered);
  std::vector<MvsView> neighbours = {views[0], views[2], views[3]};

  MvsConfig config;
  config.min_depth = 0.5;
  config.max_depth = 5.0;
  config.depth_samples = 64;

  const DepthMap estimate = estimateDepth(views[1], neighbours, config);
  ASSERT_FALSE(estimate.empty());

  const Accuracy accuracy = compare(estimate, rendered[1].truth);
  EXPECT_GT(accuracy.measured, accuracy.total / 4);
  EXPECT_LT(accuracy.median_absolute_error, 0.2)
      << "median " << accuracy.median_absolute_error;
}

/// Hypotheses are spaced in inverse depth, so the far field is sampled more
/// coarsely than the near field. That is deliberate, and this pins it.
TEST(MvsTest, NearSurfacesAreMeasuredMorePreciselyThanFarOnes) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto near_views = renderOrbit(scene, Vec3(0, 0.25, 0), 0.8, 0.3, 8);
  const auto far_views = renderOrbit(scene, Vec3(0, 0.25, 0), 2.4, 0.9, 8);

  MvsConfig config;
  config.min_depth = 0.3;
  config.max_depth = 4.0;
  config.depth_samples = 48;

  const auto estimateFor = [&](const std::vector<RenderedView>& rendered) {
    const auto views = asMvsViews(rendered);
    std::vector<MvsView> neighbours = {views[1], views[2], views[6], views[7]};
    return compare(estimateDepth(views[0], neighbours, config), rendered[0].truth);
  };

  const Accuracy near_accuracy = estimateFor(near_views);
  const Accuracy far_accuracy = estimateFor(far_views);
  ASSERT_GT(near_accuracy.measured, 0);
  ASSERT_GT(far_accuracy.measured, 0);
  // Absolute error grows with depth; what should not grow much is the error
  // *relative* to the distance, which is what inverse-depth sampling buys.
  EXPECT_LT(near_accuracy.median_absolute_error, far_accuracy.median_absolute_error + 1e-6);
}

/// An untextured surface must come back unmeasured rather than confidently
/// wrong: fusion carves a hole through the real surface for a wrong depth.
TEST(MvsTest, LeavesUntexturedSurfacesUnmeasured) {
  SyntheticScene scene;
  testing::Primitive wall;
  wall.shape = testing::Shape::Plane;
  wall.normal = Vec3(0, 0, -1);
  wall.center = Vec3(0, 0, 2.0);
  wall.texture = testing::Texture::Flat;
  scene.primitives.push_back(wall);

  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(96, 72);
  std::vector<RenderedView> rendered;
  for (double offset : {-0.15, 0.0, 0.15}) {
    const Pose pose = SyntheticScene::lookAt(Vec3(offset, 0, 0), Vec3(0, 0, 2.0));
    const auto result = scene.render(intrinsics, pose, false, true);
    rendered.push_back({result.gray, result.depth, pose, intrinsics});
  }
  const auto views = asMvsViews(rendered);
  std::vector<MvsView> neighbours = {views[0], views[2]};

  const DepthMap estimate = estimateDepth(views[1], neighbours, MvsConfig());
  const Accuracy accuracy = compare(estimate, rendered[1].truth);
  // Almost nothing should be measured on a flat untextured wall.
  if (accuracy.total > 0) {
    EXPECT_LT(static_cast<double>(accuracy.measured) / accuracy.total, 0.2)
        << accuracy.measured << " of " << accuracy.total;
  }
}

TEST(MvsTest, ReturnsNothingWithoutNeighbours) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 2, 64, 48);
  const auto views = asMvsViews(rendered);
  EXPECT_TRUE(estimateDepth(views[0], {}, MvsConfig()).empty());
}

TEST(MvsTest, ReturnsNothingWithNoBaseline) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 2, 64, 48);
  auto views = asMvsViews(rendered);
  // The same camera twice: no parallax, so no depth is recoverable and the
  // honest answer is an empty map.
  views[1].pose = views[0].pose;
  EXPECT_TRUE(estimateDepth(views[0], {views[1]}, MvsConfig()).empty());
}

TEST(MvsTest, RejectsNonsenseConfiguration) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 3, 64, 48);
  const auto views = asMvsViews(rendered);

  MvsConfig inverted;
  inverted.min_depth = 3.0;
  inverted.max_depth = 1.0;
  EXPECT_TRUE(estimateDepth(views[0], {views[1]}, inverted).empty());

  MvsConfig single_sample;
  single_sample.depth_samples = 1;
  EXPECT_TRUE(estimateDepth(views[0], {views[1]}, single_sample).empty());
}

// ---- neighbour selection ---------------------------------------------------

TEST(MvsTest, NeighbourSelectionPrefersAUsableBaseline) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 16);
  const auto views = asMvsViews(rendered);

  const std::vector<int> chosen = selectNeighbours(views, 0, 4, 1.0);
  ASSERT_EQ(chosen.size(), 4u);
  for (int index : chosen) {
    EXPECT_NE(index, 0);
    // Nothing from the far side of the orbit, which cannot see the same
    // surface at all.
    const double alignment = views[0].pose.forward().dot(views[index].pose.forward());
    EXPECT_GT(alignment, 0.4) << "index " << index;
  }
}

TEST(MvsTest, NeighbourSelectionSkipsZeroBaselineViews) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 4);
  auto views = asMvsViews(rendered);
  views[1].pose = views[0].pose;

  const std::vector<int> chosen = selectNeighbours(views, 0, 3, 1.0);
  for (int index : chosen) EXPECT_NE(index, 1);
}

TEST(MvsTest, NeighbourSelectionHandlesDegenerateInput) {
  EXPECT_TRUE(selectNeighbours({}, 0, 4, 1.0).empty());

  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const auto rendered = renderOrbit(scene, Vec3(0, 0.25, 0), 1.0, 0.3, 3, 32, 24);
  const auto views = asMvsViews(rendered);
  EXPECT_TRUE(selectNeighbours(views, -1, 4, 1.0).empty());
  EXPECT_TRUE(selectNeighbours(views, 99, 4, 1.0).empty());
  // A nonsense scene depth must not divide by zero. It falls back to one metre,
  // so the choice is identical to passing that explicitly. (Here that is the
  // empty set: a three-view orbit puts every other camera 120 degrees away,
  // which sees a different surface entirely.)
  EXPECT_EQ(selectNeighbours(views, 0, 2, 0.0), selectNeighbours(views, 0, 2, 1.0));
}

// ---- depth range from sparse points ---------------------------------------

TEST(MvsTest, DepthRangeComesFromTheVisiblePoints) {
  const SyntheticScene scene = SyntheticScene::objectOnTable();
  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(160, 120);
  const Pose pose = SyntheticScene::lookAt(Vec3(0, 0.6, -1.2), Vec3(0, 0.25, 0));
  const std::vector<Vec3> points = scene.samplePoints(400);

  double min_depth = 0.0;
  double max_depth = 0.0;
  ASSERT_TRUE(depthRangeFromPoints(points, pose, intrinsics, &min_depth, &max_depth));

  EXPECT_GT(min_depth, 0.0);
  EXPECT_LT(min_depth, max_depth);
  // The camera is about 1.3 m from the subject, so the range has to contain it
  // without being so wide that every sample is wasted.
  EXPECT_LT(min_depth, 1.3);
  EXPECT_GT(max_depth, 1.3);
  EXPECT_LT(max_depth - min_depth, 4.0);
}

TEST(MvsTest, DepthRangeIsNotThrownOffByOneBadPoint) {
  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(160, 120);
  const Pose pose = Pose::Identity();
  std::vector<Vec3> points;
  for (int i = 0; i < 100; ++i) {
    points.push_back(Vec3(0.0, 0.0, 1.0 + i * 0.002));
  }
  // One mis-triangulated point four hundred metres away, which is exactly what
  // a wide-baseline outlier produces.
  points.push_back(Vec3(0.0, 0.0, 400.0));

  double min_depth = 0.0;
  double max_depth = 0.0;
  ASSERT_TRUE(depthRangeFromPoints(points, pose, intrinsics, &min_depth, &max_depth));
  EXPECT_LT(max_depth, 10.0) << "max " << max_depth;
}

TEST(MvsTest, DepthRangeNeedsEnoughPoints) {
  const CameraIntrinsics intrinsics = testing::phoneIntrinsics(64, 48);
  double min_depth = 0.0;
  double max_depth = 0.0;
  // Too few points is reported, so the caller falls back to the configured
  // range rather than sweeping a range derived from three points.
  EXPECT_FALSE(depthRangeFromPoints({Vec3(0, 0, 1)}, Pose::Identity(), intrinsics, &min_depth,
                                    &max_depth));
  // Points behind the camera are not visible and do not count.
  std::vector<Vec3> behind;
  for (int i = 0; i < 40; ++i) behind.push_back(Vec3(0, 0, -1.0 - i * 0.01));
  EXPECT_FALSE(
      depthRangeFromPoints(behind, Pose::Identity(), intrinsics, &min_depth, &max_depth));
}

}  // namespace
}  // namespace parallax
