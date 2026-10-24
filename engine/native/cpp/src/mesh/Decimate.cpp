// Quadric error metric mesh decimation (Garland & Heckbert, SIGGRAPH '97).
//
// Each vertex accumulates a 4x4 quadric summarising the squared distance to the
// planes of its incident triangles. The cost of collapsing an edge is the value
// of the summed quadric at the optimal new position, which is available in
// closed form. Edges are collapsed cheapest-first.
//
// The implementation uses lazy deletion: collapses invalidate queue entries
// rather than removing them, and each entry carries the version counters of its
// two endpoints so stale entries are discarded when popped. That keeps the
// whole thing to one priority queue and no mutable heap bookkeeping.
#include "parallax/mesh/Mesh.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace parallax {
namespace {

inline uint64_t edgeKey(uint32_t a, uint32_t b) {
  if (a > b) std::swap(a, b);
  return (static_cast<uint64_t>(a) << 32) | b;
}

inline Vec3 toDouble(const Vec3f& v) { return Vec3(v.x(), v.y(), v.z()); }
inline Vec3f toFloat(const Vec3& v) {
  return Vec3f(static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z()));
}

struct Candidate {
  double cost = 0.0;
  uint32_t a = 0, b = 0;
  uint64_t version_a = 0, version_b = 0;
  Vec3 target = Vec3::Zero();

  // std::priority_queue is a max-heap, so invert the comparison.
  bool operator<(const Candidate& other) const { return cost > other.cost; }
};

class Decimator {
 public:
  Decimator(Mesh* mesh, bool preserve_boundary)
      : mesh_(mesh), preserve_boundary_(preserve_boundary) {}

  int run(int target_triangles);

 private:
  void buildTopology();
  void computeQuadrics();
  void markBoundaryVertices();
  /// Optimal collapse position and its quadric cost.
  void evaluate(uint32_t a, uint32_t b, Vec3* target, double* cost) const;
  bool collapseIsLegal(uint32_t a, uint32_t b, const Vec3& target) const;
  void collapse(uint32_t a, uint32_t b, const Vec3& target);
  void pushEdge(uint32_t a, uint32_t b, std::priority_queue<Candidate>* queue) const;

  Mesh* mesh_;
  bool preserve_boundary_;
  std::vector<Vec3> position_;
  std::vector<Mat4> quadric_;
  std::vector<uint8_t> alive_;
  std::vector<uint8_t> boundary_;
  std::vector<uint64_t> version_;
  std::vector<std::vector<uint32_t>> incident_;  ///< vertex -> triangle ids
  std::vector<uint32_t> tri_;
  std::vector<uint8_t> tri_alive_;
  int live_triangles_ = 0;
};

void Decimator::buildTopology() {
  const size_t n = mesh_->vertices.size();
  position_.resize(n);
  for (size_t i = 0; i < n; ++i) position_[i] = toDouble(mesh_->vertices[i]);
  alive_.assign(n, 1);
  version_.assign(n, 0);
  incident_.assign(n, {});

  tri_ = mesh_->indices;
  const size_t tri_count = tri_.size() / 3;
  tri_alive_.assign(tri_count, 1);
  live_triangles_ = static_cast<int>(tri_count);
  for (size_t t = 0; t < tri_count; ++t) {
    for (int k = 0; k < 3; ++k) incident_[tri_[t * 3 + k]].push_back(static_cast<uint32_t>(t));
  }
}

void Decimator::computeQuadrics() {
  quadric_.assign(mesh_->vertices.size(), Mat4::Zero());
  for (size_t t = 0; t < tri_alive_.size(); ++t) {
    if (!tri_alive_[t]) continue;
    const Vec3& a = position_[tri_[t * 3]];
    const Vec3& b = position_[tri_[t * 3 + 1]];
    const Vec3& c = position_[tri_[t * 3 + 2]];
    Vec3 normal = (b - a).cross(c - a);
    const double area2 = normal.norm();
    if (area2 < 1e-20) continue;
    normal /= area2;
    // Plane in homogeneous form p = (nx, ny, nz, -n.a); the squared distance of
    // a point x to the plane is (p . [x;1])^2, i.e. [x;1]^T (p p^T) [x;1].
    Vec4 plane;
    plane << normal, -normal.dot(a);
    // Weighting by twice the triangle area makes the metric scale correctly for
    // irregular tessellations, which marching tetrahedra always produces.
    const Mat4 contribution = (plane * plane.transpose()) * area2;
    for (int k = 0; k < 3; ++k) quadric_[tri_[t * 3 + k]] += contribution;
  }
}

void Decimator::markBoundaryVertices() {
  boundary_.assign(mesh_->vertices.size(), 0);
  std::unordered_map<uint64_t, int> use_count;
  use_count.reserve(tri_alive_.size() * 3);
  for (size_t t = 0; t < tri_alive_.size(); ++t) {
    if (!tri_alive_[t]) continue;
    const uint32_t v[3] = {tri_[t * 3], tri_[t * 3 + 1], tri_[t * 3 + 2]};
    for (int e = 0; e < 3; ++e) ++use_count[edgeKey(v[e], v[(e + 1) % 3])];
  }
  for (const auto& [key, count] : use_count) {
    if (count != 2) {
      boundary_[static_cast<uint32_t>(key >> 32)] = 1;
      boundary_[static_cast<uint32_t>(key & 0xFFFFFFFFu)] = 1;
    }
  }
}

void Decimator::evaluate(uint32_t a, uint32_t b, Vec3* target, double* cost) const {
  const Mat4 Q = quadric_[a] + quadric_[b];
  // Minimise v^T Q v over v. Differentiating gives A v = -c, where A is Q's
  // upper-left 3x3 block and c is its top-right 3x1 column.
  const Mat3 A = Q.topLeftCorner<3, 3>();
  const Vec3 c = Q.topRightCorner<3, 1>();

  bool solved = false;
  Vec3 best = (position_[a] + position_[b]) * 0.5;
  // A is singular whenever the incident planes are degenerate (flat or
  // perfectly symmetric neighbourhoods), which is common on the planar regions
  // a room scan is mostly made of, so the fallback matters.
  const Eigen::FullPivLU<Mat3> lu(A);
  if (lu.isInvertible() && lu.rcond() > 1e-10) {
    const Vec3 candidate = lu.solve(-c);
    if (candidate.allFinite()) {
      // Reject a solution that lands far outside the edge: on near-degenerate
      // quadrics the exact minimiser can fly off and fold the mesh.
      const double edge_length = (position_[a] - position_[b]).norm();
      if ((candidate - best).norm() <= 4.0 * edge_length + 1e-9) {
        best = candidate;
        solved = true;
      }
    }
  }
  if (!solved) {
    // Pick the cheapest of the midpoint and the two endpoints.
    const auto quadricCost = [&Q](const Vec3& v) {
      Vec4 h;
      h << v, 1.0;
      return h.dot(Q * h);
    };
    double best_cost = quadricCost(best);
    for (const Vec3& candidate : {position_[a], position_[b]}) {
      const double value = quadricCost(candidate);
      if (value < best_cost) {
        best_cost = value;
        best = candidate;
      }
    }
  }

  Vec4 h;
  h << best, 1.0;
  *target = best;
  // Clamp tiny negatives from round-off: the quadric is positive semi-definite.
  *cost = std::max(0.0, h.dot(Q * h));
}

bool Decimator::collapseIsLegal(uint32_t a, uint32_t b, const Vec3& target) const {
  if (!alive_[a] || !alive_[b] || a == b) return false;
  if (preserve_boundary_ && (boundary_[a] || boundary_[b])) return false;

  // Link condition. Collapsing a-b is manifold-safe only if the vertices
  // adjacent to both endpoints are exactly the apexes of the triangles on the
  // edge. Violating it welds together parts of the surface that only meet at a
  // point, producing non-manifold geometry no exporter can represent.
  std::unordered_set<uint32_t> ring_a;
  for (uint32_t t : incident_[a]) {
    if (!tri_alive_[t]) continue;
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = tri_[t * 3 + k];
      if (v != a) ring_a.insert(v);
    }
  }
  int shared = 0;
  std::unordered_set<uint32_t> counted;
  for (uint32_t t : incident_[b]) {
    if (!tri_alive_[t]) continue;
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = tri_[t * 3 + k];
      if (v == b || v == a) continue;
      if (ring_a.count(v) && counted.insert(v).second) ++shared;
    }
  }
  int edge_triangles = 0;
  for (uint32_t t : incident_[a]) {
    if (!tri_alive_[t]) continue;
    for (int k = 0; k < 3; ++k) {
      if (tri_[t * 3 + k] == b) {
        ++edge_triangles;
        break;
      }
    }
  }
  if (edge_triangles == 0) return false;          // not an edge of the live mesh
  if (shared != edge_triangles) return false;     // link condition violated

  // Normal flip test: no surviving triangle may invert. Folding the surface
  // over on itself is geometrically cheap under the quadric metric but visually
  // catastrophic and breaks volume computation.
  for (uint32_t endpoint : {a, b}) {
    for (uint32_t t : incident_[endpoint]) {
      if (!tri_alive_[t]) continue;
      uint32_t v[3] = {tri_[t * 3], tri_[t * 3 + 1], tri_[t * 3 + 2]};
      bool has_a = false, has_b = false;
      for (int k = 0; k < 3; ++k) {
        has_a |= v[k] == a;
        has_b |= v[k] == b;
      }
      if (has_a && has_b) continue;  // this triangle disappears in the collapse
      const Vec3 before =
          (position_[v[1]] - position_[v[0]]).cross(position_[v[2]] - position_[v[0]]);
      Vec3 p[3];
      for (int k = 0; k < 3; ++k) {
        p[k] = (v[k] == a || v[k] == b) ? target : position_[v[k]];
      }
      const Vec3 after = (p[1] - p[0]).cross(p[2] - p[0]);
      if (before.norm() < 1e-20 || after.norm() < 1e-20) continue;
      if (before.normalized().dot(after.normalized()) < 0.2) return false;
    }
  }
  return true;
}

void Decimator::collapse(uint32_t a, uint32_t b, const Vec3& target) {
  position_[a] = target;
  quadric_[a] += quadric_[b];
  alive_[b] = 0;

  for (uint32_t t : incident_[b]) {
    if (!tri_alive_[t]) continue;
    bool has_a = false;
    for (int k = 0; k < 3; ++k) {
      if (tri_[t * 3 + k] == a) has_a = true;
    }
    if (has_a) {
      // Triangle spanned the collapsed edge; it degenerates to a line.
      tri_alive_[t] = 0;
      --live_triangles_;
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      if (tri_[t * 3 + k] == b) tri_[t * 3 + k] = a;
    }
    incident_[a].push_back(t);
  }
  incident_[b].clear();

  // Drop dead triangles from a's adjacency so the lists cannot grow without
  // bound over a long decimation run.
  auto& list = incident_[a];
  list.erase(std::remove_if(list.begin(), list.end(),
                            [this](uint32_t t) { return !tri_alive_[t]; }),
             list.end());
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());

  ++version_[a];
  ++version_[b];
  // Neighbours' costs change too; bumping their versions invalidates their
  // stale queue entries so they are re-evaluated when next encountered.
  for (uint32_t t : incident_[a]) {
    for (int k = 0; k < 3; ++k) ++version_[tri_[t * 3 + k]];
  }
  if (boundary_[b]) boundary_[a] = 1;
}

void Decimator::pushEdge(uint32_t a, uint32_t b, std::priority_queue<Candidate>* queue) const {
  if (a == b || !alive_[a] || !alive_[b]) return;
  Candidate candidate;
  candidate.a = a;
  candidate.b = b;
  candidate.version_a = version_[a];
  candidate.version_b = version_[b];
  evaluate(a, b, &candidate.target, &candidate.cost);
  queue->push(candidate);
}

int Decimator::run(int target_triangles) {
  buildTopology();
  if (live_triangles_ <= target_triangles) return live_triangles_;
  computeQuadrics();
  markBoundaryVertices();

  std::priority_queue<Candidate> queue;
  {
    std::unordered_set<uint64_t> seen;
    for (size_t t = 0; t < tri_alive_.size(); ++t) {
      if (!tri_alive_[t]) continue;
      const uint32_t v[3] = {tri_[t * 3], tri_[t * 3 + 1], tri_[t * 3 + 2]};
      for (int e = 0; e < 3; ++e) {
        const uint32_t a = v[e], b = v[(e + 1) % 3];
        if (seen.insert(edgeKey(a, b)).second) pushEdge(a, b, &queue);
      }
    }
  }

  // Bound the work: every pop either collapses or discards, and each collapse
  // pushes O(ring) new entries, so the queue is finite, but a pathological mesh
  // could still thrash. The guard keeps the worst case linear-ish.
  const size_t max_pops = queue.size() * 40 + 10000;
  size_t pops = 0;

  while (!queue.empty() && live_triangles_ > target_triangles && pops++ < max_pops) {
    const Candidate candidate = queue.top();
    queue.pop();
    // Stale entry: one of the endpoints has changed since this was queued.
    if (!alive_[candidate.a] || !alive_[candidate.b]) continue;
    if (version_[candidate.a] != candidate.version_a ||
        version_[candidate.b] != candidate.version_b) {
      pushEdge(candidate.a, candidate.b, &queue);
      continue;
    }
    if (!collapseIsLegal(candidate.a, candidate.b, candidate.target)) continue;

    collapse(candidate.a, candidate.b, candidate.target);

    // Re-queue the edges of the merged vertex's new ring.
    const uint32_t a = candidate.a;
    for (uint32_t t : incident_[a]) {
      if (!tri_alive_[t]) continue;
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = tri_[t * 3 + k];
        if (v != a) pushEdge(a, v, &queue);
      }
    }
  }

  // ---- write the result back ---------------------------------------------
  const bool has_normals = mesh_->hasNormals();
  const bool has_colors = mesh_->hasColors();
  const bool has_uvs = mesh_->hasUvs();

  std::vector<uint32_t> remap(position_.size(), UINT32_MAX);
  Mesh out;
  out.vertices.reserve(live_triangles_ * 3 / 2 + 8);
  for (size_t t = 0; t < tri_alive_.size(); ++t) {
    if (!tri_alive_[t]) continue;
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = tri_[t * 3 + k];
      if (remap[v] == UINT32_MAX) {
        remap[v] = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(toFloat(position_[v]));
        if (has_colors) out.colors.push_back(mesh_->colors[v]);
        if (has_uvs) out.uvs.push_back(mesh_->uvs[v]);
      }
      out.indices.push_back(remap[v]);
    }
  }
  if (has_normals) recomputeNormals(&out);
  *mesh_ = std::move(out);
  removeDegenerateTriangles(mesh_);
  return static_cast<int>(mesh_->triangleCount());
}

}  // namespace

int decimateMesh(Mesh* mesh, int target_triangles, bool preserve_boundary) {
  if (mesh == nullptr || mesh->empty()) return 0;
  if (target_triangles <= 0) {
    mesh->clear();
    return 0;
  }
  if (static_cast<int>(mesh->triangleCount()) <= target_triangles) {
    return static_cast<int>(mesh->triangleCount());
  }
  Decimator decimator(mesh, preserve_boundary);
  return decimator.run(target_triangles);
}

}  // namespace parallax
