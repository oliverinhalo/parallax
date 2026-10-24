// Shared definitions for the Gaussian splat compute pipeline.
//
// Every equation here has a counterpart in src/splat/SplatRaster.cpp, which is
// the reference and is what the host tests check. When one changes the other
// must change with it; the names are kept identical to make that obvious.
#ifndef PARALLAX_SPLAT_COMMON_GLSL
#define PARALLAX_SPLAT_COMMON_GLSL

#define TILE_SIZE 16
#define SH_C0 0.28209479177387814
#define MAX_ALPHA 0.99
#define FRUSTUM_SLACK 1.3

// Layout of the per-splat projected state, as floats. Flat arrays rather than
// a struct so that std430 padding can never silently disagree with the host.
#define PROJ_STRIDE 16
#define PROJ_MEAN_X 0
#define PROJ_MEAN_Y 1
#define PROJ_COV_XX 2
#define PROJ_COV_XY 3
#define PROJ_COV_YY 4
#define PROJ_CONIC_X 5
#define PROJ_CONIC_Y 6
#define PROJ_CONIC_Z 7
#define PROJ_DEPTH 8
#define PROJ_RADIUS 9
#define PROJ_COLOR_R 10
#define PROJ_COLOR_G 11
#define PROJ_COLOR_B 12
#define PROJ_OPACITY 13

// meta[i]: bit 0 visible, bits 1..3 per-channel spherical-harmonic clamp.
// The tile count lives in its own contiguous buffer because the prefix sum
// that turns it into write offsets wants a plain array.
#define META_STRIDE 1
#define META_FLAGS 0
#define FLAG_VISIBLE 1u

// Screen-space gradient accumulator, per splat.
#define SCREEN_STRIDE 10
#define SCREEN_DMEAN_X 0
#define SCREEN_DMEAN_Y 1
#define SCREEN_DCONIC_X 2
#define SCREEN_DCONIC_Y 3
#define SCREEN_DCONIC_Z 4
#define SCREEN_DOPACITY 5
#define SCREEN_DCOLOR_R 6
#define SCREEN_DCOLOR_G 7
#define SCREEN_DCOLOR_B 8
#define SCREEN_DMEAN_ABS 9

/**
 * Floating-point atomic add built from an integer compare-and-swap.
 *
 * VK_EXT_shader_atomic_float is not something a mid-range Android GPU can be
 * relied on to have, and the backward pass has every tile adding into the same
 * splats. The compare-and-swap loop is in core Vulkan 1.0 and works everywhere.
 *
 * It has to be a macro rather than a function: GLSL only allows the atomic
 * built-ins to name a storage-block member or a shared variable directly, so
 * the buffer cannot be passed in as a parameter.
 */
#define ATOMIC_ADD_FLOAT(ARRAY, INDEX, VALUE)                                     \
  {                                                                               \
    uint atomic_index_ = uint(INDEX);                                             \
    float atomic_value_ = float(VALUE);                                           \
    uint expected_ = ARRAY[atomic_index_];                                        \
    uint desired_ = floatBitsToUint(uintBitsToFloat(expected_) + atomic_value_);  \
    uint previous_ = atomicCompSwap(ARRAY[atomic_index_], expected_, desired_);   \
    while (previous_ != expected_) {                                              \
      expected_ = previous_;                                                      \
      desired_ = floatBitsToUint(uintBitsToFloat(expected_) + atomic_value_);     \
      previous_ = atomicCompSwap(ARRAY[atomic_index_], expected_, desired_);      \
    }                                                                             \
  }

mat3 quaternionToMatrix(vec4 q) {
  // q is (w, x, y, z) and is normalised by the caller.
  float w = q.x, x = q.y, y = q.z, z = q.w;
  return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
              2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
              2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
}

#endif  // PARALLAX_SPLAT_COMMON_GLSL
