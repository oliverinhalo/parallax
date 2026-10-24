#include "parallax/fusion/TsdfVolume.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace parallax {
namespace {

using BlockCoord = Eigen::Matrix<int32_t, 3, 1>;

struct BlockCoordHash {
  size_t operator()(const BlockCoord& c) const {
    // Three large odd constants; the mix avoids clustering for the
    // axis-aligned, highly correlated coordinates a scan produces.
    uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(c.x())) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(c.y())) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(c.z())) * 0x165667B19E3779F9ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<size_t>(h);
  }
};
struct BlockCoordEqual {
  bool operator()(const BlockCoord& a, const BlockCoord& b) const { return a == b; }
};

struct Voxel {
  float sdf = 0.f;      ///< truncated signed distance in metres
  float weight = 0.f;   ///< accumulated confidence; 0 means unobserved
  uint8_t r = 0, g = 0, b = 0;
};

struct Block {
  BlockCoord coord = BlockCoord::Zero();
  std::array<Voxel, kBlockVoxels> voxels{};
};

/// Floor division that behaves correctly for negative operands, unlike `/`.
inline int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

inline int voxelIndex(int x, int y, int z) {
  return (z * kBlockSide + y) * kBlockSide + x;
}

constexpr uint32_t kSerializationMagic = 0x50585446u;  // "PXTF"
constexpr uint32_t kSerializationVersion = 1u;

template <typename T>
void writePod(std::vector<uint8_t>* out, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool readPod(const std::vector<uint8_t>& in, size_t* offset, T* value) {
  if (*offset + sizeof(T) > in.size()) return false;
  std::memcpy(value, in.data() + *offset, sizeof(T));
  *offset += sizeof(T);
  return true;
}

}  // namespace

struct TsdfVolume::Impl {
  std::unordered_map<BlockCoord, Block, BlockCoordHash, BlockCoordEqual> blocks;

  /// Global voxel coordinate -> (block coord, local index).
  static void split(const BlockCoord& voxel, BlockCoord* block, int* local) {
    BlockCoord bc(floorDiv(voxel.x(), kBlockSide), floorDiv(voxel.y(), kBlockSide),
                  floorDiv(voxel.z(), kBlockSide));
    *block = bc;
    *local = voxelIndex(voxel.x() - bc.x() * kBlockSide, voxel.y() - bc.y() * kBlockSide,
                        voxel.z() - bc.z() * kBlockSide);
  }

  const Voxel* find(const BlockCoord& voxel) const {
    BlockCoord bc;
    int local = 0;
    split(voxel, &bc, &local);
    const auto it = blocks.find(bc);
    if (it == blocks.end()) return nullptr;
    return &it->second.voxels[local];
  }
};

TsdfVolume::TsdfVolume(const TsdfConfig& config) : impl_(new Impl()), config_(config) {
  config_.voxel_size = std::max(1e-4f, config_.voxel_size);
  // The surface must span several voxels for the trilinear interpolation in
  // extraction to have anything to work with.
  config_.truncation = std::max(config_.truncation, config_.voxel_size * 2.5f);
}

TsdfVolume::~TsdfVolume() = default;

// Defined here rather than in the header because Impl is incomplete there.
TsdfVolume::TsdfVolume(TsdfVolume&&) noexcept = default;
TsdfVolume& TsdfVolume::operator=(TsdfVolume&&) noexcept = default;

void TsdfVolume::clear() {
  impl_->blocks.clear();
  stats_ = TsdfStats{};
}

bool TsdfVolume::integrate(const DepthMap& depth, const ColorBuffer& color,
                           const CameraIntrinsics& k, const Pose& pose) {
  if (depth.empty() || !k.valid()) return false;
  if (depth.width != k.width || depth.height != k.height) return false;
  const bool use_color =
      config_.integrate_color && !color.empty() && color.width == depth.width &&
      color.height == depth.height;

  const float voxel = config_.voxel_size;
  const float truncation = config_.truncation;
  const Pose world_from_camera = pose.inverse();

  // ---- phase 1: allocate the blocks this frame's rays pass through ---------
  //
  // Marching along each ray only within the truncation band keeps allocation
  // proportional to surface area rather than to the viewing volume.
  std::unordered_set<BlockCoord, BlockCoordHash, BlockCoordEqual> touched;
  const float block_extent = voxel * kBlockSide;
  // Sub-sampling the allocation pass is safe: a block is 8 voxels across, so
  // neighbouring pixels overwhelmingly hit blocks already found.
  const int stride = std::max(1, std::min(depth.width, depth.height) / 160);

  for (int y = 0; y < depth.height; y += stride) {
    for (int x = 0; x < depth.width; x += stride) {
      const float d = depth.at(x, y);
      if (!(d > config_.min_depth && d < config_.max_depth)) continue;
      if (depth.conf(x, y) < config_.min_confidence) continue;

      const Vec3 ray_cam = k.unproject(Vec2(x, y));
      const Vec3 direction = (world_from_camera.R * ray_cam.normalized());
      const Vec3 origin = world_from_camera.t;
      // The band is measured along the optical axis, so convert to ray length.
      const double ray_scale = ray_cam.norm();
      const double near = (d - truncation) * ray_scale;
      const double far = (d + truncation) * ray_scale;

      for (double t = std::max(0.0, near); t <= far; t += block_extent * 0.5) {
        const Vec3 p = origin + direction * t;
        touched.insert(BlockCoord(
            static_cast<int32_t>(std::floor(p.x() / block_extent)),
            static_cast<int32_t>(std::floor(p.y() / block_extent)),
            static_cast<int32_t>(std::floor(p.z() / block_extent))));
      }
    }
  }

  for (const BlockCoord& bc : touched) {
    if (impl_->blocks.count(bc)) continue;
    if (impl_->blocks.size() >= config_.max_blocks) {
      // Memory budget reached. Stop allocating but keep updating what exists,
      // so the scan degrades by losing new area rather than by crashing.
      ++stats_.block_allocation_failures;
      continue;
    }
    Block block;
    block.coord = bc;
    impl_->blocks.emplace(bc, std::move(block));
  }

  // ---- phase 2: project every voxel of the touched blocks ------------------
  for (const BlockCoord& bc : touched) {
    const auto it = impl_->blocks.find(bc);
    if (it == impl_->blocks.end()) continue;
    Block& block = it->second;

    for (int lz = 0; lz < kBlockSide; ++lz) {
      for (int ly = 0; ly < kBlockSide; ++ly) {
        for (int lx = 0; lx < kBlockSide; ++lx) {
          // Voxel centre in world coordinates.
          const Vec3 world(
              (bc.x() * kBlockSide + lx + 0.5) * voxel,
              (bc.y() * kBlockSide + ly + 0.5) * voxel,
              (bc.z() * kBlockSide + lz + 0.5) * voxel);
          const Vec3 cam = pose * world;
          if (cam.z() < config_.min_depth || cam.z() > config_.max_depth) continue;

          Vec2 px;
          if (!k.project(cam, &px)) continue;
          const int ix = static_cast<int>(std::lround(px.x()));
          const int iy = static_cast<int>(std::lround(px.y()));
          if (ix < 0 || iy < 0 || ix >= depth.width || iy >= depth.height) continue;

          const float measured = depth.at(ix, iy);
          if (!(measured > config_.min_depth && measured < config_.max_depth)) continue;
          const uint8_t confidence = depth.conf(ix, iy);
          if (confidence < config_.min_confidence) continue;

          // Signed distance along the optical axis: positive in free space in
          // front of the surface, negative behind it.
          const float sdf = measured - static_cast<float>(cam.z());
          // Beyond the truncation band behind the surface the voxel is occluded
          // and this view says nothing about it.
          if (sdf < -truncation) continue;
          const float clamped = std::max(-truncation, std::min(truncation, sdf));

          float sample_weight = static_cast<float>(confidence) / 255.0f;
          if (config_.weight_by_incidence) {
            // cos of the angle between the ray and the optical axis is a cheap
            // proxy for obliqueness; combined with the 1/z^2 falloff of depth
            // precision it reproduces the usual sensor noise model.
            const double cos_ray = cam.z() / cam.norm();
            sample_weight *= static_cast<float>(cos_ray * cos_ray);
          }
          // Depth noise grows roughly with the square of range.
          sample_weight /= std::max(1.0f, static_cast<float>(cam.z() * cam.z()));
          if (sample_weight <= 1e-6f) continue;

          Voxel& v = block.voxels[voxelIndex(lx, ly, lz)];
          const float new_weight = std::min(config_.max_weight, v.weight + sample_weight);
          if (new_weight <= 0.f) continue;
          if (v.weight <= 0.f) ++stats_.observed_voxels;
          v.sdf = (v.sdf * v.weight + clamped * sample_weight) / (v.weight + sample_weight);
          if (use_color) {
            uint8_t rgb[3];
            color.get(ix, iy, rgb);
            // Colour is blended with the same running weight, so a surface seen
            // better later gradually takes on the better observation's colour.
            const float wa = v.weight, wb = sample_weight;
            v.r = static_cast<uint8_t>((v.r * wa + rgb[0] * wb) / (wa + wb));
            v.g = static_cast<uint8_t>((v.g * wa + rgb[1] * wb) / (wa + wb));
            v.b = static_cast<uint8_t>((v.b * wa + rgb[2] * wb) / (wa + wb));
          }
          v.weight = new_weight;
        }
      }
    }
  }

  ++stats_.integrated_frames;
  stats_.allocated_blocks = impl_->blocks.size();
  stats_.memory_bytes = static_cast<double>(impl_->blocks.size()) *
                        (sizeof(Block) + sizeof(BlockCoord) + 16);
  return true;
}

bool TsdfVolume::sampleDistance(const Vec3& world_point, float* out_distance) const {
  const double voxel = config_.voxel_size;
  // Voxel centres sit at (i + 0.5) * voxel, so shift before flooring.
  const Vec3 grid = world_point / voxel - Vec3::Constant(0.5);
  const BlockCoord base(static_cast<int32_t>(std::floor(grid.x())),
                        static_cast<int32_t>(std::floor(grid.y())),
                        static_cast<int32_t>(std::floor(grid.z())));
  const double fx = grid.x() - base.x();
  const double fy = grid.y() - base.y();
  const double fz = grid.z() - base.z();

  double value = 0.0;
  double weight_sum = 0.0;
  for (int dz = 0; dz < 2; ++dz) {
    for (int dy = 0; dy < 2; ++dy) {
      for (int dx = 0; dx < 2; ++dx) {
        const Voxel* v = impl_->find(base + BlockCoord(dx, dy, dz));
        // Every one of the eight corners must be observed; interpolating across
        // a hole would fabricate a surface at the edge of the scan.
        if (v == nullptr || v->weight <= 0.f) return false;
        const double w = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy) * (dz ? fz : 1.0 - fz);
        value += w * v->sdf;
        weight_sum += w;
      }
    }
  }
  if (weight_sum < 1e-9) return false;
  *out_distance = static_cast<float>(value / weight_sum);
  return true;
}

AxisAlignedBox TsdfVolume::observedBounds() const {
  AxisAlignedBox box;
  const double voxel = config_.voxel_size;
  for (const auto& [coord, block] : impl_->blocks) {
    for (int lz = 0; lz < kBlockSide; ++lz) {
      for (int ly = 0; ly < kBlockSide; ++ly) {
        for (int lx = 0; lx < kBlockSide; ++lx) {
          if (block.voxels[voxelIndex(lx, ly, lz)].weight <= 0.f) continue;
          box.expand(Vec3((coord.x() * kBlockSide + lx + 0.5) * voxel,
                          (coord.y() * kBlockSide + ly + 0.5) * voxel,
                          (coord.z() * kBlockSide + lz + 0.5) * voxel));
        }
      }
    }
  }
  return box;
}

Mesh TsdfVolume::extractMesh() const {
  AxisAlignedBox bounds = observedBounds();
  if (!bounds.valid()) return Mesh{};
  // Pad by a voxel so surfaces touching the observed boundary are not clipped.
  const double voxel = config_.voxel_size;
  bounds.min -= Vec3::Constant(voxel);
  bounds.max += Vec3::Constant(voxel);

  const auto sample = [this](const Vec3& p, float* value, std::array<uint8_t, 3>* color) {
    if (!sampleDistance(p, value)) return false;
    if (color != nullptr) {
      const double voxel_size = config_.voxel_size;
      const BlockCoord nearest(
          static_cast<int32_t>(std::floor(p.x() / voxel_size)),
          static_cast<int32_t>(std::floor(p.y() / voxel_size)),
          static_cast<int32_t>(std::floor(p.z() / voxel_size)));
      const Voxel* v = impl_->find(nearest);
      if (v != nullptr && v->weight > 0.f) *color = {v->r, v->g, v->b};
    }
    return true;
  };
  return marchingTetrahedra(bounds, voxel, sample);
}

void TsdfVolume::extractPointCloud(std::vector<Vec3f>* out_points,
                                   std::vector<Vec3f>* out_normals,
                                   std::vector<std::array<uint8_t, 3>>* out_colors) const {
  if (out_points) out_points->clear();
  if (out_normals) out_normals->clear();
  if (out_colors) out_colors->clear();

  const double voxel = config_.voxel_size;
  // Keep voxels whose distance puts them within half a voxel of the surface.
  const float band = static_cast<float>(voxel) * 0.75f;

  for (const auto& [coord, block] : impl_->blocks) {
    for (int lz = 0; lz < kBlockSide; ++lz) {
      for (int ly = 0; ly < kBlockSide; ++ly) {
        for (int lx = 0; lx < kBlockSide; ++lx) {
          const Voxel& v = block.voxels[voxelIndex(lx, ly, lz)];
          if (v.weight <= 0.f || std::abs(v.sdf) > band) continue;
          const Vec3 centre((coord.x() * kBlockSide + lx + 0.5) * voxel,
                            (coord.y() * kBlockSide + ly + 0.5) * voxel,
                            (coord.z() * kBlockSide + lz + 0.5) * voxel);

          // Push the point onto the zero crossing along the SDF gradient, which
          // gives sub-voxel placement for free.
          //
          // Central differences are preferred, but at the edge of the observed
          // region one neighbour is missing. Falling back to a one-sided
          // difference there keeps the normal meaningful; substituting an
          // arbitrary default instead would leave a rim of points with normals
          // pointing the wrong way, which shows up immediately as black
          // speckle in a shaded point cloud.
          Vec3 gradient = Vec3::Zero();
          bool gradient_ok = true;
          for (int axis = 0; axis < 3 && gradient_ok; ++axis) {
            Vec3 step = Vec3::Zero();
            step[axis] = voxel;
            float plus = 0.f, minus = 0.f;
            const bool has_plus = sampleDistance(centre + step, &plus);
            const bool has_minus = sampleDistance(centre - step, &minus);
            if (has_plus && has_minus) {
              gradient[axis] = (plus - minus) / (2.0 * voxel);
            } else if (has_plus) {
              gradient[axis] = (plus - v.sdf) / voxel;
            } else if (has_minus) {
              gradient[axis] = (v.sdf - minus) / voxel;
            } else {
              gradient_ok = false;
            }
          }
          if (!gradient_ok || gradient.norm() < 1e-9) {
            // No usable gradient in at least one axis: the voxel is isolated,
            // so it carries no reliable surface information at all.
            continue;
          }
          // The TSDF increases towards free space, so +gradient is outward.
          const Vec3 normal = gradient.normalized();
          const Vec3 position = centre - normal * v.sdf;
          if (out_points) {
            out_points->push_back(Vec3f(static_cast<float>(position.x()),
                                        static_cast<float>(position.y()),
                                        static_cast<float>(position.z())));
          }
          if (out_normals) {
            out_normals->push_back(Vec3f(static_cast<float>(normal.x()),
                                         static_cast<float>(normal.y()),
                                         static_cast<float>(normal.z())));
          }
          if (out_colors) out_colors->push_back({v.r, v.g, v.b});
        }
      }
    }
  }
}

std::vector<uint8_t> TsdfVolume::serialize() const {
  std::vector<uint8_t> out;
  out.reserve(impl_->blocks.size() * sizeof(Block) + 128);
  writePod(&out, kSerializationMagic);
  writePod(&out, kSerializationVersion);
  writePod(&out, config_);
  writePod(&out, static_cast<uint64_t>(impl_->blocks.size()));
  for (const auto& [coord, block] : impl_->blocks) {
    writePod(&out, coord.x());
    writePod(&out, coord.y());
    writePod(&out, coord.z());
    const auto* bytes = reinterpret_cast<const uint8_t*>(block.voxels.data());
    out.insert(out.end(), bytes, bytes + sizeof(Voxel) * kBlockVoxels);
  }
  return out;
}

bool TsdfVolume::deserialize(const std::vector<uint8_t>& bytes) {
  size_t offset = 0;
  uint32_t magic = 0, version = 0;
  if (!readPod(bytes, &offset, &magic) || magic != kSerializationMagic) return false;
  if (!readPod(bytes, &offset, &version) || version != kSerializationVersion) return false;

  TsdfConfig config;
  if (!readPod(bytes, &offset, &config)) return false;
  uint64_t block_count = 0;
  if (!readPod(bytes, &offset, &block_count)) return false;
  // Reject an implausible count before it is used to reserve memory.
  if (block_count > config.max_blocks + 1 && block_count > 10000000ull) return false;

  clear();
  config_ = config;
  for (uint64_t i = 0; i < block_count; ++i) {
    int32_t x = 0, y = 0, z = 0;
    if (!readPod(bytes, &offset, &x) || !readPod(bytes, &offset, &y) ||
        !readPod(bytes, &offset, &z)) {
      clear();
      return false;
    }
    if (offset + sizeof(Voxel) * kBlockVoxels > bytes.size()) {
      clear();
      return false;
    }
    Block block;
    block.coord = BlockCoord(x, y, z);
    std::memcpy(block.voxels.data(), bytes.data() + offset, sizeof(Voxel) * kBlockVoxels);
    offset += sizeof(Voxel) * kBlockVoxels;
    for (const Voxel& v : block.voxels) {
      if (v.weight > 0.f) ++stats_.observed_voxels;
    }
    impl_->blocks.emplace(block.coord, std::move(block));
  }
  stats_.allocated_blocks = impl_->blocks.size();
  return true;
}

}  // namespace parallax
