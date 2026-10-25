#include "parallax/fusion/ExtractionGrid.h"

#include <gtest/gtest.h>

namespace parallax {
namespace {

AxisAlignedBox boxOf(double x, double y, double z) {
  AxisAlignedBox box;
  box.expand(Vec3::Zero());
  box.expand(Vec3(x, y, z));
  return box;
}

TEST(ExtractionGrid, LeavesTheVoxelSizeAloneWhenTheVolumeFits) {
  // A desk-sized object at 8 mm: about 1.7 million lattice points.
  EXPECT_EQ(extractionCellSize(boxOf(1.0, 0.8, 1.0), 0.008, kMaxLatticePoints), 0.008);
}

TEST(ExtractionGrid, CoarsensRatherThanRefusingOnARoomSizedVolume) {
  // The failure: a room scan spread by tracking drift and depth reaching across it. At 3 cm
  // this is over a billion lattice points, and the extraction used to return an empty mesh --
  // reported to the user as "no surface was found in the fused volume".
  const AxisAlignedBox room = boxOf(40.0, 20.0, 40.0);
  ASSERT_GT(latticePointsFor(room, 0.03), kMaxLatticePoints);

  const double cell = extractionCellSize(room, 0.03, kMaxLatticePoints);

  EXPECT_GT(cell, 0.03);
  EXPECT_LE(latticePointsFor(room, cell), kMaxLatticePoints);
}

TEST(ExtractionGrid, CoarsensNoFurtherThanItHasTo) {
  // A coarser mesh than asked for is a cost, so the step past the budget should be small.
  const AxisAlignedBox room = boxOf(40.0, 20.0, 40.0);

  const double cell = extractionCellSize(room, 0.03, kMaxLatticePoints);

  // Half the budget would mean having coarsened a full step too far.
  EXPECT_GT(latticePointsFor(room, cell), kMaxLatticePoints * 0.3);
}

TEST(ExtractionGrid, TheResultIsAlwaysInsideTheBudget) {
  // Including the awkward sizes, where the per-axis ceil and the closing lattice plane are
  // most of the count and the cube root alone is not enough.
  for (double metres : {0.05, 0.3, 2.0, 7.5, 60.0, 400.0}) {
    const AxisAlignedBox box = boxOf(metres, metres, metres);
    const double cell = extractionCellSize(box, 0.005, kMaxLatticePoints);
    EXPECT_LE(latticePointsFor(box, cell), kMaxLatticePoints) << metres << " m";
    EXPECT_GE(cell, 0.005) << metres << " m";
  }
}

TEST(ExtractionGrid, AnEmptyOrInvalidBoxIsNotCoarsened) {
  EXPECT_EQ(extractionCellSize(AxisAlignedBox{}, 0.01, kMaxLatticePoints), 0.01);
  EXPECT_EQ(extractionCellSize(boxOf(0.0, 0.0, 0.0), 0.01, kMaxLatticePoints), 0.01);
}

}  // namespace
}  // namespace parallax
