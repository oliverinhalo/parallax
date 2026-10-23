// Triangle mesh container and the processing operations the pipeline needs.
#pragma once

#include "parallax/core/Types.h"

#include <array>
#include <string>

namespace parallax {

/// An indexed triangle mesh. Attribute arrays are either empty or exactly as
/// long as `vertices`, which every operation here maintains.
struct Mesh {
  std::vector<Vec3f> vertices;
  std::vector<Vec3f> normals;
  std::vector<std::array<uint8_t, 3>> colors;  ///< per-vertex RGB
  std::vector<Vec2f> uvs;
  std::vector<uint32_t> indices;               ///< 3 per triangle

  size_t vertexCount() const { return vertices.size(); }
  size_t triangleCount() const { return indices.size() / 3; }
  bool empty() const { return vertices.empty() || indices.empty(); }

  bool hasNormals() const { return normals.size() == vertices.size() && !normals.empty(); }
  bool hasColors() const { return colors.size() == vertices.size() && !colors.empty(); }
  bool hasUvs() const { return uvs.size() == vertices.size() && !uvs.empty(); }

  /// Structural self-check: index bounds, attribute lengths, finite positions.
  bool isValid(std::string* reason = nullptr) const;

  void clear();
  /// Append another mesh, offsetting its indices.
  void append(const Mesh& other);
};

struct AxisAlignedBox {
  Vec3 min = Vec3::Constant(std::numeric_limits<double>::max());
  Vec3 max = Vec3::Constant(std::numeric_limits<double>::lowest());

  bool valid() const { return (max.array() >= min.array()).all(); }
  Vec3 center() const { return (min + max) * 0.5; }
  Vec3 extent() const { return valid() ? (max - min).eval() : Vec3::Zero(); }
  double diagonal() const { return valid() ? (max - min).norm() : 0.0; }
  void expand(const Vec3& p) {
    min = min.cwiseMin(p);
    max = max.cwiseMax(p);
  }
  bool contains(const Vec3& p) const {
    return valid() && (p.array() >= min.array()).all() && (p.array() <= max.array()).all();
  }
};

AxisAlignedBox meshBounds(const Mesh& mesh);

/// Total surface area in square metres.
double meshSurfaceArea(const Mesh& mesh);

/**
 * Signed volume via the divergence theorem (sum of signed tetrahedron volumes
 * from the origin). Only meaningful for a closed, consistently oriented mesh;
 * the sign reveals inverted winding.
 */
double meshSignedVolume(const Mesh& mesh);

/// Area-weighted vertex normals, recomputed from the triangles.
void recomputeNormals(Mesh* mesh);

/// Flip triangle winding and negate normals.
void flipOrientation(Mesh* mesh);

/**
 * Merge vertices closer than `tolerance` using a spatial hash, rewriting the
 * index buffer. Marching tetrahedra emits one vertex per edge crossing per
 * tetrahedron, so welding is what turns the raw output into a connected
 * surface -- without it nothing downstream (components, decimation, hole
 * filling) can see any topology at all.
 *
 * @return number of vertices removed
 */
int weldVertices(Mesh* mesh, double tolerance = 1e-6);

/// Drop triangles with repeated indices or zero area, and unreferenced vertices.
int removeDegenerateTriangles(Mesh* mesh, double min_area = 1e-14);

/// Remove vertices not referenced by any triangle. Returns the number dropped.
int removeUnreferencedVertices(Mesh* mesh);

/// Label each triangle with its connected component id (shared-edge adjacency).
/// Returns the number of components.
int labelConnectedComponents(const Mesh& mesh, std::vector<int>* out_labels);

/**
 * Keep only components whose triangle count is at least `min_triangles`, or
 * whose surface area is at least `min_area_fraction` of the largest component.
 * This is what removes the floating debris that depth noise always produces.
 *
 * @return number of triangles removed
 */
int removeSmallComponents(Mesh* mesh, int min_triangles = 64,
                          double min_area_fraction = 0.02);

/// Keep only the single largest component (by area). Used by object mode.
int keepLargestComponent(Mesh* mesh);

struct BoundaryLoop {
  std::vector<uint32_t> vertices;  ///< ordered ring of boundary vertices
  double length = 0.0;
};

/// Find boundary (hole) loops: ordered rings of edges used by one triangle only.
std::vector<BoundaryLoop> findBoundaryLoops(const Mesh& mesh);

/**
 * Fill holes whose boundary loop has at most `max_loop_vertices` vertices by
 * fan triangulation from the loop centroid. Larger openings are left alone:
 * they are usually genuine (the unseen underside of an object), and closing
 * them invents geometry the user never captured.
 *
 * @return number of loops filled
 */
int fillHoles(Mesh* mesh, int max_loop_vertices = 200);

/// True when every edge is shared by exactly two triangles.
bool isWatertight(const Mesh& mesh);

/// Euler characteristic V - E + F, for topology assertions in tests.
int eulerCharacteristic(const Mesh& mesh);

/**
 * Quadric error metric decimation (Garland & Heckbert). Collapses edges in
 * order of the quadric error they introduce until the triangle count reaches
 * `target_triangles`.
 *
 * @param preserve_boundary  when true, edges on a boundary loop are never
 *                           collapsed, so holes keep their shape
 * @return the achieved triangle count
 */
int decimateMesh(Mesh* mesh, int target_triangles, bool preserve_boundary = true);

/// Laplacian smoothing with uniform weights; `lambda` in (0,1).
void smoothMesh(Mesh* mesh, int iterations = 1, double lambda = 0.5,
                bool preserve_boundary = true);

/// Generate a chain of decimated levels of detail, each half the previous size.
std::vector<Mesh> generateLods(const Mesh& mesh, int level_count = 3,
                               double reduction = 0.5);

/// Transform all positions (and rotate normals) by a rigid+scale transform.
void transformMesh(Mesh* mesh, const Mat3& rotation, const Vec3& translation,
                   double scale = 1.0);

/// Keep only the geometry inside `box`; triangles straddling the boundary are
/// kept if their centroid is inside, which avoids re-triangulating.
int cropMesh(Mesh* mesh, const AxisAlignedBox& box);

}  // namespace parallax
