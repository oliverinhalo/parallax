// 3D Gaussian splatting data model and interchange formats.
//
// Training happens on the GPU (see docs/DECISIONS.md, ADR-009); this header
// covers the representation and the file formats, which are host-testable.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/mesh/Mesh.h"

#include <string>

namespace parallax {

/// Number of spherical-harmonic coefficients per colour channel for a degree.
/// Degree 0 is the DC term only; each further band adds 2n+1 coefficients.
inline int shCoefficientsPerChannel(int degree) {
  switch (degree) {
    case 0: return 1;
    case 1: return 4;
    case 2: return 9;
    case 3: return 16;
    default: return 1;
  }
}

/// Coefficients *excluding* the DC term, per channel.
inline int shRestPerChannel(int degree) { return shCoefficientsPerChannel(degree) - 1; }

/**
 * One anisotropic 3D Gaussian.
 *
 * Field conventions follow the original INRIA implementation, because that is
 * what the .ply interchange format stores and what every viewer expects:
 *   * `log_scale` holds log(sigma), not sigma
 *   * `opacity_logit` holds the pre-sigmoid value
 *   * `rotation` is a unit quaternion
 *   * `dc` is the degree-0 spherical-harmonic coefficient, not an sRGB colour
 * Keeping the storage convention identical to the format avoids a whole class
 * of "the splats are inside out / washed out" bugs on import and export.
 */
struct Gaussian {
  Vec3f position = Vec3f::Zero();
  Vec3f log_scale = Vec3f::Zero();
  Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
  float opacity_logit = 0.f;
  std::array<float, 3> dc{0.f, 0.f, 0.f};
  /// Higher-order SH, laid out channel-major: all R coefficients, then G, then
  /// B. This is the INRIA .ply ordering (`f_rest_*`), *not* the interleaved
  /// ordering .spz uses; the spz writer transposes.
  std::vector<float> sh_rest;
};

/// The constant that converts a degree-0 SH coefficient to linear RGB.
constexpr float kShC0 = 0.28209479177387814f;

struct GaussianCloud {
  std::vector<Gaussian> gaussians;
  int sh_degree = 0;
  /// True when the splats were trained with the antialiased rasteriser; .spz
  /// carries this as a flag and viewers need it to match the training.
  bool antialiased = false;

  size_t size() const { return gaussians.size(); }
  bool empty() const { return gaussians.empty(); }
  bool isValid(std::string* reason = nullptr) const;

  AxisAlignedBox bounds() const;
  /// Remove gaussians whose opacity or scale puts them below visibility.
  int prune(float min_opacity = 0.0045f, float max_scale = 1e3f);
  /// Sort by descending opacity, so a truncated export keeps what matters.
  void sortByOpacity();
};

/// Linear RGB in [0,1] from the DC term.
inline std::array<float, 3> gaussianBaseColor(const Gaussian& g) {
  return {std::max(0.f, std::min(1.f, 0.5f + kShC0 * g.dc[0])),
          std::max(0.f, std::min(1.f, 0.5f + kShC0 * g.dc[1])),
          std::max(0.f, std::min(1.f, 0.5f + kShC0 * g.dc[2]))};
}

inline float gaussianOpacity(const Gaussian& g) {
  return 1.0f / (1.0f + std::exp(-g.opacity_logit));
}

/**
 * Initialise a cloud from a coloured point cloud, as the pipeline does before
 * training: one isotropic gaussian per point, with the scale set from the local
 * point spacing so the initial splats just touch their neighbours.
 */
GaussianCloud initializeFromPointCloud(const std::vector<Vec3f>& points,
                                       const std::vector<std::array<uint8_t, 3>>& colors,
                                       int sh_degree = 0,
                                       float initial_opacity = 0.1f);

// ---------------------------------------------------------------- formats ---

/**
 * The standard 3DGS PLY layout: binary little-endian with properties
 * x,y,z, nx,ny,nz, f_dc_0..2, f_rest_0..N, opacity, scale_0..2, rot_0..3.
 * `rot_0` is the quaternion's w component, matching the reference implementation.
 */
bool writeGaussianPly(const std::string& path, const GaussianCloud& cloud);
bool readGaussianPly(const std::string& path, GaussianCloud* out, std::string* error = nullptr);

/**
 * The `.splat` format used by antimatter15's web viewer: a flat array of
 * 32-byte records, with no header at all.
 *   position  3 x float32   (12 bytes)
 *   scale     3 x float32   (12 bytes)  -- linear sigma, not log
 *   colour    4 x uint8     ( 4 bytes)  -- RGBA, alpha is the opacity
 *   rotation  4 x uint8     ( 4 bytes)  -- quaternion, (q * 128 + 128)
 * Higher-order spherical harmonics are not representable and are dropped.
 */
bool writeSplat(const std::string& path, const GaussianCloud& cloud);
bool readSplat(const std::string& path, GaussianCloud* out, std::string* error = nullptr);

/**
 * Niantic's `.spz` v2: a gzip stream wrapping a 16-byte header followed by
 * attribute planes (positions, scales, rotations, alphas, colours, SH).
 *
 * Verification boundary: the container layout, field widths and attribute order
 * are implemented from the published format description, and this engine's own
 * reader round-trips what its writer produces. The per-attribute quantisation
 * constants below are taken from the reference encoder's documented behaviour
 * but have *not* been checked against Niantic's decoder in this environment, so
 * cross-tool interoperability is unverified. Note also that SPZ 4 (2026) adds a
 * plaintext header and ZSTD streams; this writer targets v2, which remains the
 * widely supported version. See docs/DECISIONS.md (ADR-010).
 */
bool writeSpz(const std::string& path, const GaussianCloud& cloud, int fractional_bits = 12);
bool readSpz(const std::string& path, GaussianCloud* out, std::string* error = nullptr);

}  // namespace parallax
