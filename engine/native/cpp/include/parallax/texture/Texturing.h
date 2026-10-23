// Texture atlas generation from the captured views.
//
// Approach: per-triangle atlas. Each triangle is assigned the single best view that sees it,
// then given its own small patch in a packed atlas, which is filled by sampling that view.
//
// The alternative -- segment the mesh into charts and UV-unwrap each one -- gives better texel
// efficiency and fewer seams, but robust unwrapping of scan geometry is a research problem in
// itself: scanned meshes have ragged boundaries, thin shells and non-manifold remnants that make
// chart parameterisation fail or produce severe distortion. A per-triangle atlas cannot fail, is
// trivially parallel, and its main weakness (a seam at every edge) is largely answered by the
// seam-levelling pass below. See docs/DECISIONS.md (ADR-008).
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/mesh/Mesh.h"

namespace parallax {

/// One view available for texturing.
struct TextureView {
  ColorBuffer image;
  /// Depth for this view, used for visibility testing. May be empty, in which case visibility
  /// is decided by surface orientation alone.
  DepthMap depth;
  CameraIntrinsics intrinsics;
  Pose pose;                 ///< camera-from-world
  double sharpness = 1.0;    ///< Laplacian variance, from capture-time frame scoring
  double exposure = 128.0;   ///< mean intensity, used for exposure compensation
};

struct TexturingOptions {
  /// Atlas edge length in pixels. Must be a power of two for mipmapping to work cleanly.
  int atlas_size = 4096;
  /// Texels along the longest edge of the largest triangle. Smaller triangles get
  /// proportionally fewer, down to `min_patch_texels`.
  int max_patch_texels = 64;
  int min_patch_texels = 4;
  /// Padding around each patch, so bilinear filtering and mipmapping cannot bleed between
  /// neighbouring patches in the atlas.
  int patch_padding = 2;
  /// Reject a view whose incidence angle to the surface exceeds this.
  double max_incidence_deg = 75.0;
  /// Depth agreement tolerance for the visibility test, in metres.
  double depth_tolerance = 0.04;
  /// Compensate each view's global exposure before sampling.
  bool compensate_exposure = true;
  /// Run the seam-levelling least-squares pass.
  bool level_seams = true;
  int seam_iterations = 24;
  /// Fill texels no view could see by diffusing from their neighbours.
  bool inpaint_holes = true;
};

struct TexturingStats {
  int triangles_textured = 0;
  int triangles_without_view = 0;
  int atlas_size = 0;
  double atlas_utilisation = 0.0;
  double mean_incidence_deg = 0.0;
  double seam_residual_before = 0.0;
  double seam_residual_after = 0.0;
};

struct TexturingResult {
  /// The input mesh with per-vertex UVs added. Vertices are duplicated where a vertex belongs
  /// to triangles in different atlas patches, which with a per-triangle atlas is every vertex.
  Mesh mesh;
  ColorBuffer atlas;
  TexturingStats stats;
  bool success = false;
};

/**
 * Generate a texture atlas for `mesh` from `views`.
 *
 * The mesh's triangles are preserved exactly; only the vertex array is rebuilt (three new
 * vertices per triangle) so each triangle can carry its own UVs.
 */
TexturingResult generateTextureAtlas(const Mesh& mesh, const std::vector<TextureView>& views,
                                     const TexturingOptions& options = {});

/**
 * Score how well `view` sees a triangle. Higher is better; a non-positive score means the view
 * cannot be used at all.
 *
 * Combines three terms, each of which matters independently:
 *   * sharpness, so a blurred frame never wins over a sharp one;
 *   * incidence, because a grazing view smears texels across the surface;
 *   * distance, because a distant view has fewer pixels per unit area.
 */
double scoreViewForTriangle(const TextureView& view, const Vec3& a, const Vec3& b, const Vec3& c,
                            const TexturingOptions& options);

/// True if the triangle's centroid is visible in `view` (in front, in frame, and not occluded
/// according to the view's depth map).
bool triangleVisibleIn(const TextureView& view, const Vec3& a, const Vec3& b, const Vec3& c,
                       const TexturingOptions& options);

}  // namespace parallax
