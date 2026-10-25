#pragma once

#include "parallax/mesh/Mesh.h"

namespace parallax {

/**
 * Lattice points the surface extraction will visit at most.
 *
 * Marching tetrahedra walks a dense grid over the volume's bounds, so the cost is the bounds
 * divided by the cell size, cubed -- it has nothing to do with how much surface is in there. A
 * room scan at 3 cm voxels, spread over an area by tracking drift and a few depth samples
 * reaching across the room, asks for more than a billion.
 */
constexpr double kMaxLatticePoints = 4e8;

/**
 * The cell size to extract a surface at, coarsened if the volume is too large to walk finely.
 *
 * The extraction used to refuse: over the budget it returned an empty mesh, which is
 * indistinguishable from a volume with no surface in it and was reported to the user as
 * exactly that -- "no surface was found in the fused volume", on a volume holding nine and a
 * half million observed voxels. A coarser mesh of a large room is worth having; no mesh is not,
 * and neither is a message about the wrong thing.
 *
 * @return [voxel] when the volume fits, otherwise the smallest multiple of it that does.
 */
double extractionCellSize(const AxisAlignedBox& bounds, double voxel, double budget);

/// Lattice points `bounds` needs at `cell_size`. Doubles throughout: the point is the overflow.
double latticePointsFor(const AxisAlignedBox& bounds, double cell_size);

}  // namespace parallax
