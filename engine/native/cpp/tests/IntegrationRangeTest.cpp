#include "parallax/fusion/IntegrationRange.h"

#include <gtest/gtest.h>

#include <vector>

namespace parallax {
namespace {

/// Samples spread between `near` and `far`, which is what a depth map of a surface looks like.
std::vector<float> spread(float near_m, float far_m, int count) {
  std::vector<float> samples;
  samples.reserve(count);
  for (int i = 0; i < count; ++i) {
    const float t = static_cast<float>(i) / (count - 1);
    samples.push_back(near_m + t * (far_m - near_m));
  }
  return samples;
}

TEST(IntegrationRange, CoversTheFurthestSurfaceInTheCapture) {
  // A chair from across a room: nothing nearer than two and a half metres.
  std::vector<float> samples = spread(2.5f, 3.5f, 400);

  const float range = integrationDepthFrom(&samples, 6.0f);

  // The old fixed 1.6 m dropped every one of these samples, and the volume came back empty.
  EXPECT_GT(range, 3.5f) << range;
}

TEST(IntegrationRange, DoesNotStretchToTheFarFieldBehindTheSubject) {
  // A mug at arm's length, with a handful of samples running off down the hallway. Integrating
  // to the hallway would spend the whole voxel budget on surface nobody wants.
  std::vector<float> samples = spread(0.3f, 0.6f, 400);
  for (int i = 0; i < 40; ++i) samples.push_back(9.0f);

  const float range = integrationDepthFrom(&samples, 6.0f);

  EXPECT_LT(range, 1.5f) << range;
  EXPECT_GT(range, 0.6f) << range;
}

TEST(IntegrationRange, NeverReturnsABandTooShallowToReconstructFrom) {
  std::vector<float> samples = spread(0.05f, 0.08f, 100);

  EXPECT_GE(integrationDepthFrom(&samples, 6.0f), kMinIntegrationDepthM);
}

TEST(IntegrationRange, TheModesCeilingCapsNonsense) {
  // A depth map full of garbage must not make the volume try to span a kilometre.
  std::vector<float> samples = spread(400.0f, 900.0f, 100);

  EXPECT_EQ(integrationDepthFrom(&samples, 6.0f), 6.0f);
}

TEST(IntegrationRange, WithNoSamplesAtAllItFallsBackToTheCeiling) {
  std::vector<float> none;

  EXPECT_EQ(integrationDepthFrom(&none, 6.0f), 6.0f);
  EXPECT_EQ(integrationDepthFrom(nullptr, 6.0f), 6.0f);
}

TEST(IntegrationRange, ACeilingBelowTheUsableMinimumIsRaisedToIt) {
  std::vector<float> samples = spread(1.0f, 2.0f, 100);

  EXPECT_EQ(integrationDepthFrom(&samples, 0.1f), kMinIntegrationDepthM);
}

}  // namespace
}  // namespace parallax
