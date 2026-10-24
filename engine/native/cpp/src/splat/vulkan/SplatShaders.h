// The compiled compute shaders, embedded as SPIR-V.
//
// CMake runs glslc over src/splat/vulkan/shaders/*.comp and drops a
// `<name>.spv.inc` holding a C initialiser list into the generated include
// directory, which is why the includes below have no path.
#pragma once

#include <cstdint>

namespace parallax {
namespace shaders {


inline constexpr uint32_t kProject[] =
#include "project.spv.inc"
    ;
inline constexpr uint32_t kScanBlock[] =
#include "scan_block.spv.inc"
    ;
inline constexpr uint32_t kScanAdd[] =
#include "scan_add.spv.inc"
    ;
inline constexpr uint32_t kPairWrite[] =
#include "pair_write.spv.inc"
    ;
inline constexpr uint32_t kRadixHistogram[] =
#include "radix_histogram.spv.inc"
    ;
inline constexpr uint32_t kRadixScatter[] =
#include "radix_scatter.spv.inc"
    ;
inline constexpr uint32_t kTileRanges[] =
#include "tile_ranges.spv.inc"
    ;
inline constexpr uint32_t kForward[] =
#include "forward.spv.inc"
    ;
inline constexpr uint32_t kBackward[] =
#include "backward.spv.inc"
    ;
inline constexpr uint32_t kBackwardParams[] =
#include "backward_params.spv.inc"
    ;
inline constexpr uint32_t kAdam[] =
#include "adam.spv.inc"
    ;
inline constexpr uint32_t kLossL1[] =
#include "loss_l1.spv.inc"
    ;
inline constexpr uint32_t kSsimPrepare[] =
#include "ssim_prepare.spv.inc"
    ;
inline constexpr uint32_t kBlur[] =
#include "blur.spv.inc"
    ;
inline constexpr uint32_t kSsimTerms[] =
#include "ssim_terms.spv.inc"
    ;
inline constexpr uint32_t kSsimGradient[] =
#include "ssim_gradient.spv.inc"
    ;


}  // namespace shaders
}  // namespace parallax
