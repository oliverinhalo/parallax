#include "parallax/fusion/TsdfVolume.h"

#include <array>
#include <cmath>

namespace parallax {
namespace {

/// The eight corners of the unit cube, indexed so that corner i has bit k of i
/// set when its k-th coordinate is 1.
constexpr int kCubeCorner[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
};

/**
 * Freudenthal (Kuhn) decomposition of the cube into six tetrahedra, all sharing
 * the main diagonal 0-7.
 *
 * Every tetrahedron is {0, 7, a, b} where (a, b) walks one of the six monotone
 * lattice paths from corner 0 to corner 7. Because the decomposition is defined
 * purely by corner indices, two cubes sharing a face split that face into the
 * same two triangles, so the extracted surface has no cracks. (Decomposing into
 * five tetrahedra would require alternating the orientation between neighbouring
 * cubes, which is where the classic cracking bug comes from.)
 */
constexpr int kTetrahedra[6][4] = {
    {0, 7, 1, 3}, {0, 7, 3, 2}, {0, 7, 2, 6},
    {0, 7, 6, 4}, {0, 7, 4, 5}, {0, 7, 5, 1},
};

struct Corner {
  Vec3 position;
  float value = 0.f;
  std::array<uint8_t, 3> color{200, 200, 200};
  bool observed = false;
};

/// Linear interpolation of the zero crossing along an edge.
void interpolate(const Corner& a, const Corner& b, Vec3* out_position,
                 std::array<uint8_t, 3>* out_color) {
  const float da = a.value, db = b.value;
  const float denom = da - db;
  // Both endpoints straddle zero, so denom cannot be zero unless the field is
  // constant; guard anyway and fall back to the midpoint.
  double t = std::abs(denom) > 1e-12f ? static_cast<double>(da / denom) : 0.5;
  t = std::max(0.0, std::min(1.0, t));
  *out_position = a.position + (b.position - a.position) * t;
  for (int c = 0; c < 3; ++c) {
    (*out_color)[c] = static_cast<uint8_t>(
        std::lround(a.color[c] * (1.0 - t) + b.color[c] * t));
  }
}

/**
 * Emit a triangle, orienting it so its normal points along `outward`.
 *
 * Deriving the winding from corner-index order does not work: the six
 * tetrahedra of the Freudenthal decomposition do not all have the same
 * handedness, and the isolated-corner case permutes its three neighbours
 * differently depending on which corner is isolated. Checking the normal
 * against a geometric outward direction is independent of all of that.
 */
void emitTriangle(Mesh* mesh, const Vec3& a, const Vec3& b, const Vec3& c,
                  const std::array<uint8_t, 3>& ca, const std::array<uint8_t, 3>& cb,
                  const std::array<uint8_t, 3>& cc, const Vec3& outward) {
  // Swapping the last two vertices reverses the winding, and hence the normal.
  // Decide once and emit: re-testing the sign after a swap would see the same
  // result again (negating `outward` as well flips it back), which recurses
  // forever.
  const bool flip = (b - a).cross(c - a).dot(outward) < 0.0;
  const Vec3& v1 = flip ? c : b;
  const Vec3& v2 = flip ? b : c;
  const std::array<uint8_t, 3>& k1 = flip ? cc : cb;
  const std::array<uint8_t, 3>& k2 = flip ? cb : cc;

  const auto push = [mesh](const Vec3& p) {
    mesh->vertices.push_back(
        Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z())));
  };
  const uint32_t base = static_cast<uint32_t>(mesh->vertices.size());
  push(a);
  push(v1);
  push(v2);
  mesh->colors.push_back(ca);
  mesh->colors.push_back(k1);
  mesh->colors.push_back(k2);
  mesh->indices.push_back(base);
  mesh->indices.push_back(base + 1);
  mesh->indices.push_back(base + 2);
}

/**
 * Triangulate one tetrahedron.
 *
 * With four corners there are 16 sign patterns. Up to complement symmetry there
 * are only three distinct cases:
 *   * all four corners the same sign  -> no surface
 *   * one corner isolated (4 patterns x 2 signs) -> one triangle, cutting the
 *     three edges that meet at the isolated corner
 *   * two against two (3 patterns x 2 signs) -> a quad across the four edges
 *     joining the two pairs, emitted as two triangles
 *
 * Winding is chosen so the normal points from negative (inside) to positive
 * (outside), matching the TSDF sign convention.
 */
void triangulateTetrahedron(const Corner& c0, const Corner& c1, const Corner& c2,
                            const Corner& c3, Mesh* mesh) {
  const Corner* corners[4] = {&c0, &c1, &c2, &c3};
  for (const Corner* c : corners) {
    if (!c->observed) return;  // never invent surface across unobserved space
  }

  int mask = 0;
  for (int i = 0; i < 4; ++i) {
    if (corners[i]->value < 0.0f) mask |= (1 << i);
  }
  if (mask == 0 || mask == 15) return;

  // Partition the corners by sign.
  int negative[4], positive[4];
  int negative_count = 0, positive_count = 0;
  for (int i = 0; i < 4; ++i) {
    if (mask & (1 << i)) {
      negative[negative_count++] = i;
    } else {
      positive[positive_count++] = i;
    }
  }

  // The field increases towards free space, so the direction from the centroid
  // of the inside corners to the centroid of the outside corners is outward.
  // This is what fixes the winding, independently of corner index order.
  Vec3 inside_centroid = Vec3::Zero(), outside_centroid = Vec3::Zero();
  for (int i = 0; i < negative_count; ++i) inside_centroid += corners[negative[i]]->position;
  for (int i = 0; i < positive_count; ++i) outside_centroid += corners[positive[i]]->position;
  inside_centroid /= negative_count;
  outside_centroid /= positive_count;
  Vec3 outward = outside_centroid - inside_centroid;
  if (outward.squaredNorm() < 1e-24) return;  // degenerate tetrahedron
  outward.normalize();

  Vec3 p[4];
  std::array<uint8_t, 3> col[4];

  if (negative_count == 1 || positive_count == 1) {
    // One isolated corner: the surface cuts the three edges meeting at it.
    const bool isolated_is_negative = negative_count == 1;
    const int apex = isolated_is_negative ? negative[0] : positive[0];
    const int* others = isolated_is_negative ? positive : negative;
    const int other_count = isolated_is_negative ? positive_count : negative_count;
    for (int e = 0; e < other_count; ++e) {
      interpolate(*corners[apex], *corners[others[e]], &p[e], &col[e]);
    }
    emitTriangle(mesh, p[0], p[1], p[2], col[0], col[1], col[2], outward);
    return;
  }

  // Two against two: four cut edges forming a quad. Pairing them as
  // (n0-p0, n0-p1, n1-p1, n1-p0) walks the quad's perimeter in order, so
  // splitting on the 0-2 diagonal yields two non-overlapping triangles.
  interpolate(*corners[negative[0]], *corners[positive[0]], &p[0], &col[0]);
  interpolate(*corners[negative[0]], *corners[positive[1]], &p[1], &col[1]);
  interpolate(*corners[negative[1]], *corners[positive[1]], &p[2], &col[2]);
  interpolate(*corners[negative[1]], *corners[positive[0]], &p[3], &col[3]);
  emitTriangle(mesh, p[0], p[1], p[2], col[0], col[1], col[2], outward);
  emitTriangle(mesh, p[0], p[2], p[3], col[0], col[2], col[3], outward);
}

}  // namespace

Mesh marchingTetrahedra(
    const AxisAlignedBox& bounds, double cell_size,
    const std::function<bool(const Vec3&, float*, std::array<uint8_t, 3>*)>& sample) {
  Mesh mesh;
  if (!bounds.valid() || cell_size <= 0.0) return mesh;

  const Vec3 extent = bounds.extent();
  const int nx = static_cast<int>(std::ceil(extent.x() / cell_size)) + 1;
  const int ny = static_cast<int>(std::ceil(extent.y() / cell_size)) + 1;
  const int nz = static_cast<int>(std::ceil(extent.z() / cell_size)) + 1;
  if (nx < 2 || ny < 2 || nz < 2) return mesh;
  // Guard against a pathological bounds/cell_size combination asking for more
  // lattice points than could possibly fit in memory.
  const double total = static_cast<double>(nx) * ny * nz;
  if (total > 4e8) return mesh;

  // Sample two z-slabs at a time: the cube between slab z and z+1 needs only
  // those two, so memory is O(nx * ny) rather than O(nx * ny * nz).
  const size_t slab = static_cast<size_t>(nx) * ny;
  std::vector<Corner> lower(slab), upper(slab);

  const auto fillSlab = [&](int z, std::vector<Corner>* out) {
    for (int y = 0; y < ny; ++y) {
      for (int x = 0; x < nx; ++x) {
        Corner& corner = (*out)[static_cast<size_t>(y) * nx + x];
        corner.position = bounds.min + Vec3(x, y, z) * cell_size;
        corner.observed = sample(corner.position, &corner.value, &corner.color);
      }
    }
  };

  fillSlab(0, &lower);
  for (int z = 0; z + 1 < nz; ++z) {
    fillSlab(z + 1, &upper);

    for (int y = 0; y + 1 < ny; ++y) {
      for (int x = 0; x + 1 < nx; ++x) {
        // Gather the eight cube corners, indexed to match kCubeCorner.
        Corner cube[8];
        bool all_observed = true;
        for (int c = 0; c < 8; ++c) {
          const int cx = x + kCubeCorner[c][0];
          const int cy = y + kCubeCorner[c][1];
          const std::vector<Corner>& source = kCubeCorner[c][2] ? upper : lower;
          cube[c] = source[static_cast<size_t>(cy) * nx + cx];
          all_observed &= cube[c].observed;
        }
        if (!all_observed) continue;

        // Cheap rejection: if every corner has the same sign no tetrahedron of
        // this cube can contain the surface.
        bool any_negative = false, any_positive = false;
        for (const Corner& c : cube) {
          any_negative |= c.value < 0.0f;
          any_positive |= c.value >= 0.0f;
        }
        if (!(any_negative && any_positive)) continue;

        for (const auto& tet : kTetrahedra) {
          triangulateTetrahedron(cube[tet[0]], cube[tet[1]], cube[tet[2]], cube[tet[3]], &mesh);
        }
      }
    }
    lower.swap(upper);
  }

  // The extractor emits each triangle with its own vertices; welding turns that
  // soup into a connected surface. The tolerance is a small fraction of a cell,
  // which is far below the spacing of genuinely distinct crossings.
  weldVertices(&mesh, cell_size * 1e-4);
  recomputeNormals(&mesh);
  return mesh;
}

}  // namespace parallax
