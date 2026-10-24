// Turning point clouds and splat fields into ordinary triangle geometry.
//
// The viewer has exactly one way to draw something: load a GLB. That is a
// deliberate constraint. Points and gaussians both need their own shader to be
// drawn properly, and a custom Filament material has to be compiled by `matc`
// at build time -- another tool in the chain, and one whose output could not be
// tested here at all. Converting both to triangles with vertex colours instead
// puts the whole of the interesting work in this file, where it is testable,
// and leaves the renderer with nothing to get wrong.
//
// The cost is stated plainly in docs/DECISIONS.md (ADR-013): splats drawn as
// oriented quads with a baked radial falloff are a good likeness rather than a
// correct rasterisation, and their blend order is fixed when the file is
// written instead of per frame.
#pragma once

#include "parallax/mesh/Mesh.h"
#include "parallax/splat/GaussianCloud.h"

namespace parallax {

struct PointPreviewOptions {
  /**
   * Radius of each disc, in metres.
   *
   * Zero asks for it to be chosen from the cloud's own density, which is
   * almost always what you want: a fixed radius leaves gaps in a sparse scan
   * and turns a dense one into a solid lump.
   */
  float radius_metres = 0.f;
  /// Triangles per point. Three is a disc you cannot tell from a circle at
  /// scan density; more is wasted on something a few pixels across.
  int segments = 3;
  /// Cap on the output, sampled evenly rather than truncated.
  int max_points = 400000;
};

/**
 * One small disc per point, facing the point's normal.
 *
 * Facing the normal rather than the camera is what makes this work as a static
 * file: a surface sampled densely enough reads as a surface from any angle,
 * and a point whose normal is unknown gets a disc facing up, which is the
 * least wrong guess for a scan of a room or a table.
 */
Mesh buildPointCloudPreview(const std::vector<Vec3f>& points, const std::vector<Vec3f>& normals,
                            const std::vector<std::array<uint8_t, 3>>& colors,
                            const PointPreviewOptions& options = {});

struct SplatPreviewOptions {
  /// Standard deviations of the gaussian the quad covers. Past three the
  /// falloff is below what eight-bit alpha can represent.
  float extent_sigma = 2.5f;
  /// Cap on the output. Splats are sorted by opacity before truncation, so
  /// what goes is what contributed least.
  int max_gaussians = 200000;
  /// Minimum opacity worth drawing.
  float min_opacity = 0.02f;
};

/**
 * Two triangles per gaussian, spanning its two widest axes.
 *
 * The third axis is dropped, which is the same approximation the real
 * rasteriser makes when it projects the covariance: a gaussian is a flattened
 * ellipsoid and what you see is its cross-section. The quad's UVs run over
 * [0,1] so the radial alpha texture from [splatFalloffTexture] gives the
 * gaussian its soft edge.
 */
Mesh buildSplatPreview(const GaussianCloud& cloud, const SplatPreviewOptions& options = {});

/**
 * The alpha ramp a splat quad is textured with: `exp(-0.5 * r^2 / sigma^2)`
 * sampled over the quad, as a square greyscale image written into the RGB
 * channels so it can ride in an ordinary glTF base-colour texture.
 */
ColorBuffer splatFalloffTexture(int size = 64, float extent_sigma = 2.5f);

}  // namespace parallax
