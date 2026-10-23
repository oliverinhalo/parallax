#include "parallax/mesh/Mesh.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace parallax {
namespace {

/// Key for the undirected edge (a, b).
inline uint64_t edgeKey(uint32_t a, uint32_t b) {
  if (a > b) std::swap(a, b);
  return (static_cast<uint64_t>(a) << 32) | b;
}

inline Vec3 toDouble(const Vec3f& v) { return Vec3(v.x(), v.y(), v.z()); }
inline Vec3f toFloat(const Vec3& v) {
  return Vec3f(static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z()));
}

double triangleArea(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
  return 0.5 * (toDouble(b) - toDouble(a)).cross(toDouble(c) - toDouble(a)).norm();
}

/// Spatial hash for vertex welding: quantise to a grid of side `cell`.
struct GridKey {
  int64_t x, y, z;
  bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridKeyHash {
  size_t operator()(const GridKey& k) const {
    uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(k.z) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    return static_cast<size_t>(h);
  }
};

}  // namespace

// ------------------------------------------------------------------- basics

bool Mesh::isValid(std::string* reason) const {
  const auto fail = [reason](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (indices.size() % 3 != 0) return fail("index count is not a multiple of 3");
  for (uint32_t i : indices) {
    if (i >= vertices.size()) return fail("index out of range");
  }
  if (!normals.empty() && normals.size() != vertices.size()) {
    return fail("normal count does not match vertex count");
  }
  if (!colors.empty() && colors.size() != vertices.size()) {
    return fail("color count does not match vertex count");
  }
  if (!uvs.empty() && uvs.size() != vertices.size()) {
    return fail("uv count does not match vertex count");
  }
  for (const Vec3f& v : vertices) {
    if (!std::isfinite(v.x()) || !std::isfinite(v.y()) || !std::isfinite(v.z())) {
      return fail("non-finite vertex position");
    }
  }
  if (reason) reason->clear();
  return true;
}

void Mesh::clear() {
  vertices.clear();
  normals.clear();
  colors.clear();
  uvs.clear();
  indices.clear();
}

void Mesh::append(const Mesh& other) {
  const uint32_t offset = static_cast<uint32_t>(vertices.size());
  // Attribute arrays must stay either empty or full length, so pad whichever
  // side is missing an attribute the other has.
  const bool want_normals = hasNormals() || other.hasNormals();
  const bool want_colors = hasColors() || other.hasColors();
  const bool want_uvs = hasUvs() || other.hasUvs();
  if (want_normals && normals.size() != vertices.size()) normals.resize(vertices.size(), Vec3f::Zero());
  if (want_colors && colors.size() != vertices.size()) {
    colors.resize(vertices.size(), {200, 200, 200});
  }
  if (want_uvs && uvs.size() != vertices.size()) uvs.resize(vertices.size(), Vec2f::Zero());

  vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());
  if (want_normals) {
    if (other.hasNormals()) {
      normals.insert(normals.end(), other.normals.begin(), other.normals.end());
    } else {
      normals.resize(vertices.size(), Vec3f::Zero());
    }
  }
  if (want_colors) {
    if (other.hasColors()) {
      colors.insert(colors.end(), other.colors.begin(), other.colors.end());
    } else {
      colors.resize(vertices.size(), {200, 200, 200});
    }
  }
  if (want_uvs) {
    if (other.hasUvs()) {
      uvs.insert(uvs.end(), other.uvs.begin(), other.uvs.end());
    } else {
      uvs.resize(vertices.size(), Vec2f::Zero());
    }
  }
  indices.reserve(indices.size() + other.indices.size());
  for (uint32_t i : other.indices) indices.push_back(i + offset);
}

AxisAlignedBox meshBounds(const Mesh& mesh) {
  AxisAlignedBox box;
  for (const Vec3f& v : mesh.vertices) box.expand(toDouble(v));
  return box;
}

double meshSurfaceArea(const Mesh& mesh) {
  double area = 0.0;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    area += triangleArea(mesh.vertices[mesh.indices[t]], mesh.vertices[mesh.indices[t + 1]],
                         mesh.vertices[mesh.indices[t + 2]]);
  }
  return area;
}

double meshSignedVolume(const Mesh& mesh) {
  // Sum of signed volumes of tetrahedra (origin, a, b, c). For a closed,
  // outward-oriented surface this telescopes to the enclosed volume.
  double volume = 0.0;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec3 a = toDouble(mesh.vertices[mesh.indices[t]]);
    const Vec3 b = toDouble(mesh.vertices[mesh.indices[t + 1]]);
    const Vec3 c = toDouble(mesh.vertices[mesh.indices[t + 2]]);
    volume += a.dot(b.cross(c)) / 6.0;
  }
  return volume;
}

void recomputeNormals(Mesh* mesh) {
  if (mesh == nullptr) return;
  mesh->normals.assign(mesh->vertices.size(), Vec3f::Zero());
  for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
    const uint32_t ia = mesh->indices[t], ib = mesh->indices[t + 1], ic = mesh->indices[t + 2];
    const Vec3 a = toDouble(mesh->vertices[ia]);
    const Vec3 b = toDouble(mesh->vertices[ib]);
    const Vec3 c = toDouble(mesh->vertices[ic]);
    // The un-normalised cross product has magnitude 2 * area, which gives the
    // area weighting for free.
    const Vec3 n = (b - a).cross(c - a);
    const Vec3f nf = toFloat(n);
    mesh->normals[ia] += nf;
    mesh->normals[ib] += nf;
    mesh->normals[ic] += nf;
  }
  for (Vec3f& n : mesh->normals) {
    const float len = n.norm();
    n = len > 1e-20f ? (n / len).eval() : Vec3f(0.f, 1.f, 0.f);
  }
}

void flipOrientation(Mesh* mesh) {
  if (mesh == nullptr) return;
  for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
    std::swap(mesh->indices[t + 1], mesh->indices[t + 2]);
  }
  for (Vec3f& n : mesh->normals) n = -n;
}

void transformMesh(Mesh* mesh, const Mat3& rotation, const Vec3& translation, double scale) {
  if (mesh == nullptr) return;
  const Mat3 rs = rotation * scale;
  for (Vec3f& v : mesh->vertices) v = toFloat(rs * toDouble(v) + translation);
  // Normals transform by the rotation only; uniform scale does not affect them.
  for (Vec3f& n : mesh->normals) n = toFloat((rotation * toDouble(n)).normalized());
}

// -------------------------------------------------------------- compaction

int removeUnreferencedVertices(Mesh* mesh) {
  if (mesh == nullptr || mesh->vertices.empty()) return 0;
  std::vector<int> remap(mesh->vertices.size(), -1);
  for (uint32_t i : mesh->indices) {
    if (i < remap.size()) remap[i] = 0;
  }
  uint32_t next = 0;
  for (size_t i = 0; i < remap.size(); ++i) {
    if (remap[i] == 0) remap[i] = static_cast<int>(next++);
  }
  const int removed = static_cast<int>(mesh->vertices.size()) - static_cast<int>(next);
  if (removed <= 0) return 0;

  const bool has_normals = mesh->hasNormals();
  const bool has_colors = mesh->hasColors();
  const bool has_uvs = mesh->hasUvs();
  std::vector<Vec3f> vertices(next), normals;
  std::vector<std::array<uint8_t, 3>> colors;
  std::vector<Vec2f> uvs;
  if (has_normals) normals.resize(next);
  if (has_colors) colors.resize(next);
  if (has_uvs) uvs.resize(next);

  for (size_t i = 0; i < remap.size(); ++i) {
    if (remap[i] < 0) continue;
    const size_t d = static_cast<size_t>(remap[i]);
    vertices[d] = mesh->vertices[i];
    if (has_normals) normals[d] = mesh->normals[i];
    if (has_colors) colors[d] = mesh->colors[i];
    if (has_uvs) uvs[d] = mesh->uvs[i];
  }
  for (uint32_t& i : mesh->indices) i = static_cast<uint32_t>(remap[i]);

  mesh->vertices = std::move(vertices);
  mesh->normals = std::move(normals);
  mesh->colors = std::move(colors);
  mesh->uvs = std::move(uvs);
  return removed;
}

int weldVertices(Mesh* mesh, double tolerance) {
  if (mesh == nullptr || mesh->vertices.empty()) return 0;
  const double cell = std::max(tolerance, 1e-12);

  const bool has_normals = mesh->hasNormals();
  const bool has_colors = mesh->hasColors();
  const bool has_uvs = mesh->hasUvs();

  std::unordered_map<GridKey, std::vector<uint32_t>, GridKeyHash> buckets;
  buckets.reserve(mesh->vertices.size() * 2);
  std::vector<uint32_t> remap(mesh->vertices.size());

  std::vector<Vec3f> vertices;
  std::vector<Vec3f> normals;
  std::vector<std::array<uint8_t, 3>> colors;
  std::vector<Vec2f> uvs;
  vertices.reserve(mesh->vertices.size());

  const double tol_sq = tolerance * tolerance;
  for (size_t i = 0; i < mesh->vertices.size(); ++i) {
    const Vec3 p = toDouble(mesh->vertices[i]);
    const GridKey key{static_cast<int64_t>(std::floor(p.x() / cell)),
                      static_cast<int64_t>(std::floor(p.y() / cell)),
                      static_cast<int64_t>(std::floor(p.z() / cell))};

    // Search the 27 neighbouring cells: a point within `tolerance` can fall
    // into an adjacent cell when it sits near a cell boundary.
    int found = -1;
    for (int dz = -1; dz <= 1 && found < 0; ++dz) {
      for (int dy = -1; dy <= 1 && found < 0; ++dy) {
        for (int dx = -1; dx <= 1 && found < 0; ++dx) {
          const auto it = buckets.find(GridKey{key.x + dx, key.y + dy, key.z + dz});
          if (it == buckets.end()) continue;
          for (uint32_t candidate : it->second) {
            if ((toDouble(vertices[candidate]) - p).squaredNorm() <= tol_sq) {
              found = static_cast<int>(candidate);
              break;
            }
          }
        }
      }
    }
    if (found >= 0) {
      remap[i] = static_cast<uint32_t>(found);
      continue;
    }
    const uint32_t id = static_cast<uint32_t>(vertices.size());
    vertices.push_back(mesh->vertices[i]);
    if (has_normals) normals.push_back(mesh->normals[i]);
    if (has_colors) colors.push_back(mesh->colors[i]);
    if (has_uvs) uvs.push_back(mesh->uvs[i]);
    buckets[key].push_back(id);
    remap[i] = id;
  }

  const int removed = static_cast<int>(mesh->vertices.size()) - static_cast<int>(vertices.size());
  for (uint32_t& i : mesh->indices) i = remap[i];
  mesh->vertices = std::move(vertices);
  mesh->normals = std::move(normals);
  mesh->colors = std::move(colors);
  mesh->uvs = std::move(uvs);
  removeDegenerateTriangles(mesh);
  return removed;
}

int removeDegenerateTriangles(Mesh* mesh, double min_area) {
  if (mesh == nullptr) return 0;
  const size_t before = mesh->triangleCount();
  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());
  for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
    const uint32_t a = mesh->indices[t], b = mesh->indices[t + 1], c = mesh->indices[t + 2];
    if (a == b || b == c || a == c) continue;
    if (triangleArea(mesh->vertices[a], mesh->vertices[b], mesh->vertices[c]) <= min_area) {
      continue;
    }
    kept.push_back(a);
    kept.push_back(b);
    kept.push_back(c);
  }
  mesh->indices = std::move(kept);
  return static_cast<int>(before - mesh->triangleCount());
}

// ------------------------------------------------------------- components

int labelConnectedComponents(const Mesh& mesh, std::vector<int>* out_labels) {
  const size_t tri_count = mesh.triangleCount();
  out_labels->assign(tri_count, -1);
  if (tri_count == 0) return 0;

  // Edge -> incident triangles. A manifold edge has two; non-manifold edges are
  // tolerated and simply connect more than two triangles.
  std::unordered_map<uint64_t, std::vector<uint32_t>> edge_map;
  edge_map.reserve(tri_count * 3);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1],
                   c = mesh.indices[t * 3 + 2];
    edge_map[edgeKey(a, b)].push_back(static_cast<uint32_t>(t));
    edge_map[edgeKey(b, c)].push_back(static_cast<uint32_t>(t));
    edge_map[edgeKey(c, a)].push_back(static_cast<uint32_t>(t));
  }

  int components = 0;
  std::queue<uint32_t> frontier;
  for (size_t seed = 0; seed < tri_count; ++seed) {
    if ((*out_labels)[seed] >= 0) continue;
    const int label = components++;
    (*out_labels)[seed] = label;
    frontier.push(static_cast<uint32_t>(seed));
    while (!frontier.empty()) {
      const uint32_t t = frontier.front();
      frontier.pop();
      const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1],
                     c = mesh.indices[t * 3 + 2];
      for (const uint64_t key : {edgeKey(a, b), edgeKey(b, c), edgeKey(c, a)}) {
        const auto it = edge_map.find(key);
        if (it == edge_map.end()) continue;
        for (uint32_t neighbour : it->second) {
          if ((*out_labels)[neighbour] >= 0) continue;
          (*out_labels)[neighbour] = label;
          frontier.push(neighbour);
        }
      }
    }
  }
  return components;
}

int removeSmallComponents(Mesh* mesh, int min_triangles, double min_area_fraction) {
  if (mesh == nullptr || mesh->empty()) return 0;
  std::vector<int> labels;
  const int components = labelConnectedComponents(*mesh, &labels);
  if (components <= 1) return 0;

  std::vector<int> counts(components, 0);
  std::vector<double> areas(components, 0.0);
  for (size_t t = 0; t < labels.size(); ++t) {
    ++counts[labels[t]];
    areas[labels[t]] += triangleArea(mesh->vertices[mesh->indices[t * 3]],
                                     mesh->vertices[mesh->indices[t * 3 + 1]],
                                     mesh->vertices[mesh->indices[t * 3 + 2]]);
  }
  const double largest_area = *std::max_element(areas.begin(), areas.end());

  const size_t before = mesh->triangleCount();
  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());
  for (size_t t = 0; t < labels.size(); ++t) {
    const int label = labels[t];
    // Keep a component if it is big enough in either measure: triangle count
    // alone would discard a large, coarsely tessellated wall.
    const bool keep = counts[label] >= min_triangles ||
                      (largest_area > 0.0 && areas[label] >= min_area_fraction * largest_area);
    if (!keep) continue;
    kept.push_back(mesh->indices[t * 3]);
    kept.push_back(mesh->indices[t * 3 + 1]);
    kept.push_back(mesh->indices[t * 3 + 2]);
  }
  mesh->indices = std::move(kept);
  removeUnreferencedVertices(mesh);
  return static_cast<int>(before - mesh->triangleCount());
}

int keepLargestComponent(Mesh* mesh) {
  if (mesh == nullptr || mesh->empty()) return 0;
  std::vector<int> labels;
  const int components = labelConnectedComponents(*mesh, &labels);
  if (components <= 1) return 0;

  std::vector<double> areas(components, 0.0);
  for (size_t t = 0; t < labels.size(); ++t) {
    areas[labels[t]] += triangleArea(mesh->vertices[mesh->indices[t * 3]],
                                     mesh->vertices[mesh->indices[t * 3 + 1]],
                                     mesh->vertices[mesh->indices[t * 3 + 2]]);
  }
  const int best =
      static_cast<int>(std::max_element(areas.begin(), areas.end()) - areas.begin());

  const size_t before = mesh->triangleCount();
  std::vector<uint32_t> kept;
  for (size_t t = 0; t < labels.size(); ++t) {
    if (labels[t] != best) continue;
    kept.push_back(mesh->indices[t * 3]);
    kept.push_back(mesh->indices[t * 3 + 1]);
    kept.push_back(mesh->indices[t * 3 + 2]);
  }
  mesh->indices = std::move(kept);
  removeUnreferencedVertices(mesh);
  return static_cast<int>(before - mesh->triangleCount());
}

// ---------------------------------------------------------------- boundary

std::vector<BoundaryLoop> findBoundaryLoops(const Mesh& mesh) {
  std::vector<BoundaryLoop> loops;
  const size_t tri_count = mesh.triangleCount();
  if (tri_count == 0) return loops;

  // Count how many triangles use each undirected edge. Boundary edges are used
  // exactly once. Directed edges let us orient the loop consistently.
  std::unordered_map<uint64_t, int> use_count;
  std::unordered_map<uint32_t, std::vector<uint32_t>> next_of;
  use_count.reserve(tri_count * 3);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t v[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
    for (int e = 0; e < 3; ++e) ++use_count[edgeKey(v[e], v[(e + 1) % 3])];
  }
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t v[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = v[e], b = v[(e + 1) % 3];
      if (use_count[edgeKey(a, b)] == 1) next_of[a].push_back(b);
    }
  }

  std::unordered_set<uint64_t> visited;
  for (const auto& [start, successors] : next_of) {
    for (uint32_t first : successors) {
      if (visited.count(edgeKey(start, first))) continue;
      BoundaryLoop loop;
      uint32_t current = start, nxt = first;
      // Walk the boundary until it closes or dead-ends (non-manifold input).
      for (size_t guard = 0; guard <= next_of.size() + 2; ++guard) {
        if (visited.count(edgeKey(current, nxt))) break;
        visited.insert(edgeKey(current, nxt));
        loop.vertices.push_back(current);
        loop.length += (toDouble(mesh.vertices[nxt]) - toDouble(mesh.vertices[current])).norm();
        current = nxt;
        if (current == start) break;
        const auto it = next_of.find(current);
        if (it == next_of.end() || it->second.empty()) break;
        // Prefer an unvisited outgoing edge so a vertex shared by two loops
        // does not merge them.
        uint32_t chosen = it->second.front();
        for (uint32_t candidate : it->second) {
          if (!visited.count(edgeKey(current, candidate))) {
            chosen = candidate;
            break;
          }
        }
        nxt = chosen;
      }
      if (loop.vertices.size() >= 3) loops.push_back(std::move(loop));
    }
  }
  return loops;
}

bool isWatertight(const Mesh& mesh) {
  if (mesh.empty()) return false;
  std::unordered_map<uint64_t, int> use_count;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const uint32_t v[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
    for (int e = 0; e < 3; ++e) ++use_count[edgeKey(v[e], v[(e + 1) % 3])];
  }
  for (const auto& [key, count] : use_count) {
    if (count != 2) return false;
  }
  return true;
}

int eulerCharacteristic(const Mesh& mesh) {
  std::unordered_set<uint64_t> edges;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const uint32_t v[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
    for (int e = 0; e < 3; ++e) edges.insert(edgeKey(v[e], v[(e + 1) % 3]));
  }
  return static_cast<int>(mesh.vertexCount()) - static_cast<int>(edges.size()) +
         static_cast<int>(mesh.triangleCount());
}

int fillHoles(Mesh* mesh, int max_loop_vertices) {
  if (mesh == nullptr || mesh->empty()) return 0;
  const std::vector<BoundaryLoop> loops = findBoundaryLoops(*mesh);
  int filled = 0;

  for (const BoundaryLoop& loop : loops) {
    const size_t n = loop.vertices.size();
    if (n < 3 || n > static_cast<size_t>(max_loop_vertices)) continue;

    // Fan from the loop centroid. A centroid fan is not the minimal-area patch,
    // but it is guaranteed non-self-intersecting for the near-planar loops that
    // depth dropouts actually produce, and it adds exactly one vertex.
    Vec3 centroid = Vec3::Zero();
    for (uint32_t v : loop.vertices) centroid += toDouble(mesh->vertices[v]);
    centroid /= static_cast<double>(n);

    const uint32_t center = static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.push_back(toFloat(centroid));
    if (mesh->normals.size() == center) mesh->normals.push_back(Vec3f(0.f, 1.f, 0.f));
    if (mesh->colors.size() == center) {
      // Average the loop colour so the patch is not visually obvious.
      int r = 0, g = 0, b = 0;
      for (uint32_t v : loop.vertices) {
        r += mesh->colors[v][0];
        g += mesh->colors[v][1];
        b += mesh->colors[v][2];
      }
      mesh->colors.push_back({static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n),
                              static_cast<uint8_t>(b / n)});
    }
    if (mesh->uvs.size() == center) mesh->uvs.push_back(Vec2f::Zero());

    for (size_t i = 0; i < n; ++i) {
      const uint32_t a = loop.vertices[i];
      const uint32_t b = loop.vertices[(i + 1) % n];
      // The boundary walk follows the direction the one-sided triangles use, so
      // reversing it here gives the patch the matching outward winding.
      mesh->indices.push_back(center);
      mesh->indices.push_back(b);
      mesh->indices.push_back(a);
    }
    ++filled;
  }
  if (filled > 0) {
    removeDegenerateTriangles(mesh);
    if (mesh->hasNormals()) recomputeNormals(mesh);
  }
  return filled;
}

// ------------------------------------------------------------- smoothing

void smoothMesh(Mesh* mesh, int iterations, double lambda, bool preserve_boundary) {
  if (mesh == nullptr || mesh->empty()) return;
  lambda = std::max(0.0, std::min(1.0, lambda));

  // Adjacency and boundary flags.
  std::vector<std::vector<uint32_t>> neighbours(mesh->vertices.size());
  std::unordered_map<uint64_t, int> use_count;
  for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
    const uint32_t v[3] = {mesh->indices[t], mesh->indices[t + 1], mesh->indices[t + 2]};
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = v[e], b = v[(e + 1) % 3];
      ++use_count[edgeKey(a, b)];
      neighbours[a].push_back(b);
      neighbours[b].push_back(a);
    }
  }
  for (auto& list : neighbours) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }
  std::vector<uint8_t> on_boundary(mesh->vertices.size(), 0);
  for (const auto& [key, count] : use_count) {
    if (count == 1) {
      on_boundary[static_cast<uint32_t>(key >> 32)] = 1;
      on_boundary[static_cast<uint32_t>(key & 0xFFFFFFFF)] = 1;
    }
  }

  for (int it = 0; it < iterations; ++it) {
    std::vector<Vec3f> updated = mesh->vertices;
    for (size_t v = 0; v < mesh->vertices.size(); ++v) {
      if (neighbours[v].empty()) continue;
      if (preserve_boundary && on_boundary[v]) continue;
      Vec3 mean = Vec3::Zero();
      for (uint32_t n : neighbours[v]) mean += toDouble(mesh->vertices[n]);
      mean /= static_cast<double>(neighbours[v].size());
      const Vec3 original = toDouble(mesh->vertices[v]);
      updated[v] = toFloat(original + (mean - original) * lambda);
    }
    mesh->vertices = std::move(updated);
  }
  if (mesh->hasNormals()) recomputeNormals(mesh);
}

int cropMesh(Mesh* mesh, const AxisAlignedBox& box) {
  if (mesh == nullptr || mesh->empty() || !box.valid()) return 0;
  const size_t before = mesh->triangleCount();
  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());
  for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
    const Vec3 a = toDouble(mesh->vertices[mesh->indices[t]]);
    const Vec3 b = toDouble(mesh->vertices[mesh->indices[t + 1]]);
    const Vec3 c = toDouble(mesh->vertices[mesh->indices[t + 2]]);
    if (!box.contains((a + b + c) / 3.0)) continue;
    kept.push_back(mesh->indices[t]);
    kept.push_back(mesh->indices[t + 1]);
    kept.push_back(mesh->indices[t + 2]);
  }
  mesh->indices = std::move(kept);
  removeUnreferencedVertices(mesh);
  return static_cast<int>(before - mesh->triangleCount());
}

std::vector<Mesh> generateLods(const Mesh& mesh, int level_count, double reduction) {
  std::vector<Mesh> lods;
  if (mesh.empty() || level_count <= 0) return lods;
  reduction = std::max(0.05, std::min(0.95, reduction));
  Mesh current = mesh;
  for (int i = 0; i < level_count; ++i) {
    const int target =
        std::max(4, static_cast<int>(current.triangleCount() * reduction));
    Mesh next = current;
    decimateMesh(&next, target);
    if (next.triangleCount() >= current.triangleCount()) break;  // no further progress
    lods.push_back(next);
    current = std::move(next);
  }
  return lods;
}

}  // namespace parallax
