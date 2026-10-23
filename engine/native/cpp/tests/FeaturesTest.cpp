#include "parallax/features/Features.h"

#include <gtest/gtest.h>

#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A synthetic checkerboard with a known corner lattice.
GrayBuffer checkerboard(int w, int h, int cell) {
  GrayBuffer img(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      img.at(x, y) = (((x / cell) + (y / cell)) & 1) ? 230 : 25;
    }
  }
  return img;
}

GrayBuffer blur(const GrayBuffer& in, int passes) {
  GrayBuffer out = in;
  for (int i = 0; i < passes; ++i) out = gaussianBlur5(out.view());
  return out;
}

}  // namespace

TEST(Filters, GaussianBlurPreservesConstantRegions) {
  GrayBuffer flat(40, 30);
  for (auto& p : flat.pixels) p = 128;
  const GrayBuffer out = gaussianBlur5(flat.view());
  for (auto p : out.pixels) EXPECT_EQ(p, 128);
}

TEST(Filters, GaussianBlurReducesHighFrequencyEnergy) {
  const GrayBuffer sharp = checkerboard(96, 96, 4);
  const GrayBuffer soft = gaussianBlur5(sharp.view());
  EXPECT_LT(laplacianVariance(soft.view()), laplacianVariance(sharp.view()));
}

TEST(Filters, DownsampleHalvesDimensions) {
  const GrayBuffer in = checkerboard(64, 48, 8);
  const GrayBuffer out = downsampleHalf(in.view());
  EXPECT_EQ(out.width, 32);
  EXPECT_EQ(out.height, 24);
}

TEST(Filters, ResizeBilinearHitsRequestedSizeAndKeepsMeanLevel) {
  GrayBuffer in(64, 64);
  for (auto& p : in.pixels) p = 100;
  const GrayBuffer out = resizeBilinear(in.view(), 21, 37);
  EXPECT_EQ(out.width, 21);
  EXPECT_EQ(out.height, 37);
  for (auto p : out.pixels) EXPECT_NEAR(p, 100, 1);
}

TEST(Pyramid, LevelsShrinkMonotonicallyAndScalesAreConsistent) {
  const GrayBuffer base = checkerboard(320, 240, 10);
  const ImagePyramid pyr(base.view(), 4, 1.2);
  ASSERT_GE(pyr.levels(), 2);
  EXPECT_EQ(pyr.level(0).width, 320);
  EXPECT_DOUBLE_EQ(pyr.scaleToBase(0), 1.0);
  for (int i = 1; i < pyr.levels(); ++i) {
    EXPECT_LT(pyr.level(i).width, pyr.level(i - 1).width);
    EXPECT_GT(pyr.scaleToBase(i), pyr.scaleToBase(i - 1));
    // Scale must map level coordinates back to level-0 coordinates.
    EXPECT_NEAR(pyr.level(i).width * pyr.scaleToBase(i), 320.0, 1.5);
  }
}

TEST(Pyramid, StopsBeforeDegenerateLevels) {
  const GrayBuffer base = checkerboard(40, 40, 4);
  const ImagePyramid pyr(base.view(), 8, 1.2);
  for (int i = 0; i < pyr.levels(); ++i) {
    EXPECT_GE(pyr.level(i).width, 32);
    EXPECT_GE(pyr.level(i).height, 32);
  }
}

TEST(Fast, FiresOnConvexCornersAndNotOnFlatAreas) {
  // A single bright square on a dark field: its corners are "wedge" corners,
  // which is exactly what the FAST arc test is designed to find.
  GrayBuffer img(120, 120);
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 120; ++x) {
      img.at(x, y) = (x >= 40 && x < 80 && y >= 40 && y < 80) ? 235 : 20;
    }
  }
  EXPECT_GE(fastScore(img.view(), 40, 40, 20), 9) << "top-left corner of the square";
  EXPECT_GE(fastScore(img.view(), 79, 79, 20), 9) << "bottom-right corner of the square";
  EXPECT_LT(fastScore(img.view(), 60, 60, 20), 9) << "interior of the square is flat";
  EXPECT_LT(fastScore(img.view(), 20, 20, 20), 9) << "background is flat";
  EXPECT_LT(fastScore(img.view(), 60, 40, 20), 9) << "a straight edge is not a corner";
}

TEST(Fast, DoesNotFireOnSaddleJunctionsButHarrisDoes) {
  // Documents a real property of the FAST arc test rather than a defect: at a
  // checkerboard X-junction the circle crosses four alternating wedges, so the
  // longest contiguous bright-or-dark arc is about four pixels, far below the
  // 9 the test requires. The Harris response, which looks at the structure
  // tensor instead, scores such saddle points strongly. The pipeline therefore
  // relies on Harris for scoring and ranking, and on FAST only as a cheap
  // candidate generator.
  const GrayBuffer img = checkerboard(120, 120, 12);
  EXPECT_LT(fastScore(img.view(), 36, 36, 20), 9);
  EXPECT_GT(harrisResponse(img.view(), 36, 36), harrisResponse(img.view(), 42, 42));
}

TEST(Fast, ReturnsZeroNearTheBorder) {
  const GrayBuffer img = checkerboard(60, 60, 6);
  EXPECT_EQ(fastScore(img.view(), 1, 1, 20), 0);
  EXPECT_EQ(fastScore(img.view(), 58, 58, 20), 0);
}

TEST(Fast, HigherThresholdDetectsFewerCorners) {
  const GrayBuffer img = blur(checkerboard(200, 200, 16), 1);
  FeatureParams low;
  low.fast_threshold = 10;
  low.min_fast_threshold = 10;
  FeatureParams high;
  high.fast_threshold = 90;
  high.min_fast_threshold = 90;
  EXPECT_GE(detectFast(img.view(), low).size(), detectFast(img.view(), high).size());
}

TEST(Harris, ScoresCornersAboveEdgesAndFlatRegions) {
  const GrayBuffer img = checkerboard(120, 120, 12);
  const double corner = harrisResponse(img.view(), 36, 36);
  const double edge = harrisResponse(img.view(), 36, 42);   // on a cell boundary
  const double flat = harrisResponse(img.view(), 42, 42);   // inside a cell
  EXPECT_GT(corner, edge);
  EXPECT_GT(corner, flat);
  EXPECT_NEAR(flat, 0.0, 1e-9);
}

TEST(Orientation, TracksRotationOfAnAsymmetricPatch) {
  // A bright half-plane: the intensity centroid points into the bright side,
  // so the reported angle must follow the patch as we flip the gradient.
  const int n = 81;
  GrayBuffer right(n, n), left(n, n);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      right.at(x, y) = x > n / 2 ? 240 : 20;
      left.at(x, y) = x < n / 2 ? 240 : 20;
    }
  }
  const double a_right = intensityCentroidAngle(right.view(), n / 2, n / 2);
  const double a_left = intensityCentroidAngle(left.view(), n / 2, n / 2);
  EXPECT_NEAR(a_right, 0.0, 0.05);
  EXPECT_NEAR(std::abs(a_left), kPi, 0.05);
}

TEST(Brief, IdenticalPatchesGiveIdenticalDescriptors) {
  const GrayBuffer img = checkerboard(200, 200, 11);
  const Descriptor a = computeBrief(img.view(), 100.0, 100.0, 0.3);
  const Descriptor b = computeBrief(img.view(), 100.0, 100.0, 0.3);
  EXPECT_EQ(a.distance(b), 0);
}

TEST(Brief, DifferentPatchesGiveDistantDescriptors) {
  const GrayBuffer img = checkerboard(200, 200, 11);
  const Descriptor a = computeBrief(img.view(), 100.0, 100.0, 0.0);
  const Descriptor b = computeBrief(img.view(), 100.0 + 5.5, 100.0 + 5.5, 0.0);
  // Half a cell away the pattern inverts, so the descriptors should differ a lot.
  EXPECT_GT(a.distance(b), 40);
}

TEST(Brief, DescriptorIsDeterministicAcrossRuns) {
  // The sampling pattern is generated at first use; make sure it is stable.
  const GrayBuffer img = checkerboard(128, 128, 9);
  const Descriptor first = computeBrief(img.view(), 64.0, 64.0, 1.1);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(computeBrief(img.view(), 64.0, 64.0, 1.1).distance(first), 0);
  }
}

TEST(DetectAndDescribe, ProducesMatchingKeypointAndDescriptorCounts) {
  const GrayBuffer img = checkerboard(320, 240, 14);
  FeatureParams p;
  p.max_features = 300;
  const FeatureSet fs = detectAndDescribe(img.view(), p);
  EXPECT_FALSE(fs.empty());
  EXPECT_EQ(fs.keypoints.size(), fs.descriptors.size());
  EXPECT_LE(fs.keypoints.size(), 300u);
  for (const Keypoint& kp : fs.keypoints) {
    EXPECT_GE(kp.pt.x(), 0.0);
    EXPECT_GE(kp.pt.y(), 0.0);
    EXPECT_LE(kp.pt.x(), 320.0);
    EXPECT_LE(kp.pt.y(), 240.0);
    EXPECT_GE(kp.octave, 0);
  }
}

TEST(DetectAndDescribe, SpreadsFeaturesAcrossTheImage) {
  // A field of bright squares (genuine convex corners, unlike a checkerboard's
  // saddle points) laid out densely in the top-left quadrant and sparsely
  // elsewhere. Grid binning must stop the dense quadrant taking the whole
  // budget.
  GrayBuffer img(320, 320);
  for (auto& px : img.pixels) px = 20;
  auto drawDots = [&img](int x0, int y0, int x1, int y1, int size, int pitch) {
    for (int y = y0; y < y1; y += pitch) {
      for (int x = x0; x < x1; x += pitch) {
        for (int dy = 0; dy < size && y + dy < y1; ++dy) {
          for (int dx = 0; dx < size && x + dx < x1; ++dx) img.at(x + dx, y + dy) = 235;
        }
      }
    }
  };
  drawDots(0, 0, 160, 160, 3, 8);        // dense quadrant
  drawDots(160, 0, 320, 160, 7, 26);     // sparse
  drawDots(0, 160, 160, 320, 7, 26);     // sparse
  drawDots(160, 160, 320, 320, 7, 26);   // sparse

  FeatureParams p;
  p.max_features = 400;
  const FeatureSet fs = detectAndDescribe(img.view(), p);
  ASSERT_GT(fs.size(), 50u);
  int in_busy_quadrant = 0;
  for (const Keypoint& kp : fs.keypoints) {
    if (kp.pt.x() < 160 && kp.pt.y() < 160) ++in_busy_quadrant;
  }
  const double fraction = static_cast<double>(in_busy_quadrant) / fs.size();
  // The dense quadrant is one quarter of the image but holds far more
  // candidates; without binning it would be ~1.0.
  EXPECT_LT(fraction, 0.6) << "dense quadrant soaked up the budget";
  EXPECT_GT(fraction, 0.1) << "dense quadrant should still be represented";
}

TEST(DetectAndDescribe, HandlesDegenerateInput) {
  EXPECT_TRUE(detectAndDescribe(GrayImage{}).empty());
  GrayBuffer tiny(4, 4);
  EXPECT_TRUE(detectAndDescribe(tiny.view()).empty());
  GrayBuffer flat(200, 200);
  for (auto& p : flat.pixels) p = 120;
  EXPECT_TRUE(detectAndDescribe(flat.view()).empty());
}

TEST(Matching, RecoversCorrespondencesUnderSmallTranslation) {
  // Use a noise-textured render so the patches are locally unique; a perfect
  // checkerboard is deliberately ambiguous and would defeat any descriptor.
  SyntheticScene scene = SyntheticScene::objectOnTable();
  const CameraIntrinsics k = phoneIntrinsics(480, 360);
  const Pose a = SyntheticScene::lookAt(Vec3(0.9, 0.5, 0.9), Vec3(0, 0.2, 0));
  const Pose b = SyntheticScene::lookAt(Vec3(0.95, 0.5, 0.86), Vec3(0, 0.2, 0));

  FeatureParams p;
  p.max_features = 600;
  const FeatureSet fa = detectAndDescribe(scene.render(k, a, false, false).gray.view(), p);
  const FeatureSet fb = detectAndDescribe(scene.render(k, b, false, false).gray.view(), p);
  ASSERT_GT(fa.size(), 80u);
  ASSERT_GT(fb.size(), 80u);

  const std::vector<Match> matches = matchFeatures(fa, fb);
  EXPECT_GT(matches.size(), 30u) << "too few matches across a 4 degree baseline";

  // Matched keypoints should be spatially close given the tiny camera motion.
  int plausible = 0;
  for (const Match& m : matches) {
    const double d = (fa.keypoints[m.query].pt - fb.keypoints[m.train].pt).norm();
    if (d < 40.0) ++plausible;
  }
  EXPECT_GT(static_cast<double>(plausible) / matches.size(), 0.6);
}

TEST(Matching, CrossCheckYieldsUniqueAssignments) {
  SyntheticScene scene = SyntheticScene::objectOnTable();
  const CameraIntrinsics k = phoneIntrinsics(400, 300);
  const FeatureSet fa = detectAndDescribe(
      scene.render(k, SyntheticScene::lookAt(Vec3(0.8, 0.5, 0.8), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const FeatureSet fb = detectAndDescribe(
      scene.render(k, SyntheticScene::lookAt(Vec3(0.85, 0.5, 0.75), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const std::vector<Match> matches = matchFeatures(fa, fb, MatchParams{64, 0.8, true});

  std::vector<int> seen_query, seen_train;
  for (const Match& m : matches) {
    EXPECT_EQ(std::count(seen_query.begin(), seen_query.end(), m.query), 0);
    EXPECT_EQ(std::count(seen_train.begin(), seen_train.end(), m.train), 0);
    seen_query.push_back(m.query);
    seen_train.push_back(m.train);
  }
}

TEST(Matching, EmptyInputsProduceNoMatches) {
  FeatureSet empty;
  const GrayBuffer img = checkerboard(100, 100, 8);
  const FeatureSet real = detectAndDescribe(img.view());
  EXPECT_TRUE(matchFeatures(empty, real).empty());
  EXPECT_TRUE(matchFeatures(real, empty).empty());
  EXPECT_TRUE(matchFeatures(empty, empty).empty());
}

TEST(Matching, SortedByAscendingDistance) {
  SyntheticScene scene = SyntheticScene::objectOnTable();
  const CameraIntrinsics k = phoneIntrinsics(400, 300);
  const FeatureSet fa = detectAndDescribe(
      scene.render(k, SyntheticScene::lookAt(Vec3(0.8, 0.5, 0.8), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const FeatureSet fb = detectAndDescribe(
      scene.render(k, SyntheticScene::lookAt(Vec3(0.86, 0.5, 0.74), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const std::vector<Match> matches = matchFeatures(fa, fb);
  for (size_t i = 1; i < matches.size(); ++i) {
    EXPECT_LE(matches[i - 1].distance, matches[i].distance);
  }
}

// ---------------------------------------------------------------- frame score

TEST(FrameScoring, SharpImageScoresHigherThanBlurred) {
  // Fine detail (4 px cells) is what motion blur destroys first, and what the
  // sharpness gate is there to catch.
  const GrayBuffer sharp = checkerboard(240, 240, 4);
  const GrayBuffer soft = blur(sharp, 6);
  const FrameQuality qs = scoreFrame(sharp.view());
  const FrameQuality qb = scoreFrame(soft.view());
  EXPECT_GT(qs.sharpness, qb.sharpness);
  EXPECT_TRUE(qs.acceptable);
  EXPECT_FALSE(qb.acceptable) << "heavily blurred frame should be rejected";
}

TEST(FrameScoring, DetectsUnderAndOverExposure) {
  GrayBuffer dark(120, 120), bright(120, 120);
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 120; ++x) {
      dark.at(x, y) = (((x / 8) + (y / 8)) & 1) ? 6 : 0;
      bright.at(x, y) = (((x / 8) + (y / 8)) & 1) ? 255 : 250;
    }
  }
  const FrameQuality qd = scoreFrame(dark.view());
  const FrameQuality qb = scoreFrame(bright.view());
  EXPECT_LT(qd.exposure, 35.0);
  EXPECT_FALSE(qd.acceptable);
  EXPECT_GT(qb.exposure, 225.0);
  EXPECT_FALSE(qb.acceptable);
  EXPECT_GT(qb.clipped_high, 0.2);
  EXPECT_GT(qd.clipped_low, 0.2);
}

TEST(FrameScoring, FlatFrameIsRejectedForLowContrast) {
  GrayBuffer flat(120, 120);
  for (auto& p : flat.pixels) p = 128;
  const FrameQuality q = scoreFrame(flat.view());
  EXPECT_NEAR(q.contrast, 0.0, 1e-9);
  EXPECT_FALSE(q.acceptable);
}

TEST(FrameScoring, HandlesInvalidImage) {
  const FrameQuality q = scoreFrame(GrayImage{});
  EXPECT_FALSE(q.acceptable);
  EXPECT_EQ(q.sharpness, 0.0);
}

TEST(DifferenceHash, IdenticalImagesHashEqual) {
  const GrayBuffer img = checkerboard(200, 200, 13);
  EXPECT_EQ(differenceHash(img.view()), differenceHash(img.view()));
}

TEST(DifferenceHash, NearDuplicatesAreCloseAndDifferentScenesAreFar) {
  SyntheticScene scene = SyntheticScene::objectOnTable();
  const CameraIntrinsics k = phoneIntrinsics(320, 240);
  const uint64_t h0 = differenceHash(
      scene.render(k, SyntheticScene::lookAt(Vec3(1.0, 0.5, 0.0), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const uint64_t h_near = differenceHash(
      scene.render(k, SyntheticScene::lookAt(Vec3(1.005, 0.5, 0.01), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  const uint64_t h_far = differenceHash(
      scene.render(k, SyntheticScene::lookAt(Vec3(0.0, 0.5, 1.0), Vec3(0, 0.2, 0)), false, false)
          .gray.view());
  EXPECT_LT(hammingDistance64(h0, h_near), 10) << "tiny motion should look like a duplicate";
  EXPECT_GT(hammingDistance64(h0, h_far), hammingDistance64(h0, h_near));
}
