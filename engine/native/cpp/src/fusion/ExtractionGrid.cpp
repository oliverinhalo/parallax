#include "parallax/fusion/ExtractionGrid.h"

#include <algorithm>
#include <cmath>

namespace parallax {

namespace {
/// Each step is a 33% coarser cell, so the loop ends in a couple of turns at worst.
constexpr double kCoarsenStep = 1.33;
}  // namespace

double latticePointsFor(const AxisAlignedBox& bounds, double cell_size) {
  if (cell_size <= 0.0) return 0.0;
  const Vec3 extent = bounds.extent();
  const double nx = std::ceil(std::max(0.0, extent.x()) / cell_size) + 1.0;
  const double ny = std::ceil(std::max(0.0, extent.y()) / cell_size) + 1.0;
  const double nz = std::ceil(std::max(0.0, extent.z()) / cell_size) + 1.0;
  return nx * ny * nz;
}

double extractionCellSize(const AxisAlignedBox& bounds, double voxel, double budget) {
  if (voxel <= 0.0 || budget <= 0.0) return voxel;
  if (latticePointsFor(bounds, voxel) <= budget) return voxel;

  // The count falls with the cube of the cell size, so the needed factor is the cube root of
  // how far over budget the fine grid is. Rounded up and re-checked rather than trusted: the
  // ceil() per axis and the +1 for the closing lattice plane both matter at small extents,
  // and being over budget here is the failure this function exists to prevent.
  const double over = latticePointsFor(bounds, voxel) / budget;
  double cell = voxel * std::max(1.0, std::cbrt(over));
  while (latticePointsFor(bounds, cell) > budget) cell *= kCoarsenStep;
  return cell;
}

}  // namespace parallax
