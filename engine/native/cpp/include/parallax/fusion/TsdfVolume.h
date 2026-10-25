// Truncated signed distance field fusion with spatial hashing.
//
// Depth maps from many views are integrated into a sparse voxel volume, then
// the zero level set is extracted as a triangle mesh. Hashing 8^3 voxel blocks
// rather than allocating a dense grid is what makes room- and area-scale
// capture possible: a 10 x 10 x 3 m room at 1 cm resolution is 3 x 10^8 dense
// voxels (~3.6 GB) but only a thin shell of them is ever touched.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/geometry/SE3.h"
#include "parallax/mesh/Mesh.h"

#include <functional>
#include <memory>
#include <unordered_map>

namespace parallax {

struct TsdfConfig {
  /// Voxel edge length in metres. 0.004-0.01 for objects, 0.02-0.04 for rooms.
  float voxel_size = 0.008f;
  /// Truncation distance. Must be a few voxels wide or the surface is too thin
  /// to interpolate across; too wide and thin structures merge.
  float truncation = 0.03f;
  float min_depth = 0.15f;
  float max_depth = 5.0f;
  /// Saturating weight. Capping it keeps the surface responsive to later, better
  /// views instead of being frozen by the first hundred frames.
  float max_weight = 48.0f;
  /// Reject depth samples whose confidence byte is below this (ARCore supplies
  /// confidence; a monocular network does not, and reports 255).
  uint8_t min_confidence = 32;
  bool integrate_color = true;
  /**
   * Weight samples by the angle between the viewing ray and the local surface.
   * Grazing observations are much noisier, and down-weighting them visibly
   * reduces the ripple that otherwise appears on walls scanned from the side.
   */
  bool weight_by_incidence = true;
  /// Hard cap on allocated blocks, derived from the device memory budget.
  size_t max_blocks = 200000;
};

/// Dimensions of one hashed block.
constexpr int kBlockSide = 8;
constexpr int kBlockVoxels = kBlockSide * kBlockSide * kBlockSide;

struct TsdfStats {
  size_t allocated_blocks = 0;
  size_t observed_voxels = 0;
  size_t integrated_frames = 0;
  size_t dropped_samples = 0;
  size_t block_allocation_failures = 0;
  double memory_bytes = 0.0;
};

/// Sparse TSDF volume.
class TsdfVolume {
 public:
  explicit TsdfVolume(const TsdfConfig& config = {});
  ~TsdfVolume();
  /// Copying a volume would duplicate hundreds of megabytes of voxel data,
  /// which is never what a caller means; moving is cheap and is supported.
  TsdfVolume(const TsdfVolume&) = delete;
  TsdfVolume& operator=(const TsdfVolume&) = delete;
  TsdfVolume(TsdfVolume&&) noexcept;
  TsdfVolume& operator=(TsdfVolume&&) noexcept;

  const TsdfConfig& config() const { return config_; }
  const TsdfStats& stats() const { return stats_; }

  /**
   * Integrate one depth frame.
   *
   * @param depth  metric depth along the optical axis
   * @param color  optional RGB, same dimensions as depth (may be empty)
   * @param k      intrinsics matching the depth resolution
   * @param pose   camera-from-world pose of the frame
   * @return false if the frame was rejected outright (bad pose or dimensions)
   */
  bool integrate(const DepthMap& depth, const ColorBuffer& color, const CameraIntrinsics& k,
                 const Pose& pose);

  /// Trilinearly interpolated signed distance. Returns false outside the
  /// observed region.
  bool sampleDistance(const Vec3& world_point, float* out_distance) const;

  /// Extract the zero level set as a triangle mesh (welded, with normals).
  /**
   * Extract the surface.
   *
   * @param cell_size_used optional out-parameter: the cell size the extraction actually used,
   *   which is the voxel size unless the volume was too large to walk that finely. The caller
   *   reports it, because a mesh coarser than the preset asked for is something the user should
   *   be told rather than left to wonder about.
   */
  Mesh extractMesh(double* cell_size_used = nullptr) const;

  /// Dense coloured point cloud of surface-adjacent voxels.
  void extractPointCloud(std::vector<Vec3f>* out_points, std::vector<Vec3f>* out_normals,
                         std::vector<std::array<uint8_t, 3>>* out_colors) const;

  /// Bounding box of all observed voxels.
  AxisAlignedBox observedBounds() const;

  void clear();

  /// Serialise to a byte buffer, for pipeline checkpointing. The format is
  /// versioned and little-endian; it is an internal checkpoint, not an
  /// interchange format.
  std::vector<uint8_t> serialize() const;
  bool deserialize(const std::vector<uint8_t>& bytes);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TsdfConfig config_;
  TsdfStats stats_;
};

/**
 * Extract an isosurface from a scalar field sampled on a regular lattice.
 *
 * Uses marching *tetrahedra* on the Freudenthal 6-tetrahedron decomposition of
 * each cube rather than marching cubes. The tetrahedral case table has 16
 * entries that follow directly from the sign pattern, where marching cubes
 * needs a 256-entry hand-tabulated triangle table whose correctness cannot be
 * checked by inspection. Because every cube is split the same way, adjacent
 * tetrahedra agree on their shared faces and the result is crack-free. The cost
 * is roughly 1.8x the triangle count, which the quadric decimator removes.
 * See docs/DECISIONS.md (ADR-006).
 *
 * @param sample  returns false where the field is unobserved
 */
Mesh marchingTetrahedra(
    const AxisAlignedBox& bounds, double cell_size,
    const std::function<bool(const Vec3&, float*, std::array<uint8_t, 3>*)>& sample);

}  // namespace parallax
