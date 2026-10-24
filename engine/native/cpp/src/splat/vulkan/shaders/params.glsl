// The per-dispatch constant block, shared by every stage so that one upload
// serves the whole frame.
#ifndef PARALLAX_SPLAT_PARAMS_GLSL
#define PARALLAX_SPLAT_PARAMS_GLSL

layout(set = 0, binding = 0, std140) uniform ViewParams {
  /// Camera-from-world: rotation in the upper-left 3x3, translation in column 3.
  mat4 camera_from_world;
  /// Camera centre in world coordinates, xyz.
  vec4 camera_center;
  /// fx, fy, cx, cy
  vec4 intrinsics;
  /// width, height, gaussian_count, sh_degree
  ivec4 size;
  /// screen_dilation, min_alpha, transmittance_cutoff, ssim_weight
  vec4 options;
  /// tile columns, tile rows, sh_rest stride, total (splat, tile) pairs
  ivec4 tiles;
  /// Background colour, rgb.
  vec4 background;
  /// near, far, 1/(far-near), unused -- the range the sort key quantises over.
  vec4 depth_range;
}
params;

#define VIEW_FX params.intrinsics.x
#define VIEW_FY params.intrinsics.y
#define VIEW_CX params.intrinsics.z
#define VIEW_CY params.intrinsics.w
#define VIEW_WIDTH params.size.x
#define VIEW_HEIGHT params.size.y
#define GAUSSIAN_COUNT params.size.z
#define SH_DEGREE params.size.w
#define SCREEN_DILATION params.options.x
#define MIN_ALPHA params.options.y
#define TRANSMITTANCE_CUTOFF params.options.z
#define TILE_COLS params.tiles.x
#define TILE_ROWS params.tiles.y
#define SH_REST_STRIDE params.tiles.z
#define PAIR_COUNT params.tiles.w
#define DEPTH_NEAR params.depth_range.x
#define DEPTH_FAR params.depth_range.y

/**
 * Pack a (tile, depth) pair into one 32-bit sort key.
 *
 * Fourteen bits of tile index and eighteen of depth, rather than a 64-bit key
 * and twice the radix passes. Eighteen bits over the scene's depth range is
 * about a quarter of a millimetre at room scale, which is far finer than the
 * ordering of two overlapping splats can be told apart; fourteen bits of tile
 * covers a 2048x2048 render, and the host refuses anything larger.
 */
#define TILE_KEY_BITS 14
#define DEPTH_KEY_BITS 18
#define DEPTH_KEY_MAX 262143u

uint packSortKey(uint tile, float depth) {
  float span = max(DEPTH_FAR - DEPTH_NEAR, 1e-6);
  float normalised = clamp((depth - DEPTH_NEAR) / span, 0.0, 1.0);
  uint quantised = uint(round(normalised * float(DEPTH_KEY_MAX)));
  return (tile << DEPTH_KEY_BITS) | quantised;
}

uint tileOfSortKey(uint key) { return key >> DEPTH_KEY_BITS; }

mat3 viewRotation() { return mat3(params.camera_from_world); }
vec3 viewTranslation() { return params.camera_from_world[3].xyz; }

#endif  // PARALLAX_SPLAT_PARAMS_GLSL
