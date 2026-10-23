#include "parallax/floorplan/FloorPlan.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <queue>
#include <sstream>

#include "parallax/exporters/MeshExport.h"

namespace parallax {
namespace {

inline Vec3 toDouble(const Vec3f& v) { return Vec3(v.x(), v.y(), v.z()); }

/// A 2D occupancy grid over the floor plane.
struct Grid {
  int width = 0, height = 0;
  double resolution = 0.05;
  Vec2d origin = Vec2d::Zero();  ///< world (plane) coordinate of cell (0,0)
  std::vector<int32_t> counts;

  bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
  int32_t at(int x, int y) const { return inside(x, y) ? counts[static_cast<size_t>(y) * width + x] : 0; }
  void add(int x, int y) {
    if (inside(x, y)) ++counts[static_cast<size_t>(y) * width + x];
  }
  Vec2d cellCentre(int x, int y) const {
    return origin + Vec2d((x + 0.5) * resolution, (y + 0.5) * resolution);
  }
  void toCell(const Vec2d& p, int* x, int* y) const {
    *x = static_cast<int>(std::floor((p.x() - origin.x()) / resolution));
    *y = static_cast<int>(std::floor((p.y() - origin.y()) / resolution));
  }
};

/// Trace the boundary of the interior region with the Moore neighbourhood
/// (square tracing) algorithm, yielding a closed loop of cell corners.
std::vector<Vec2d> traceBoundary(const Grid& grid, const std::vector<uint8_t>& interior) {
  const auto isInterior = [&](int x, int y) {
    return grid.inside(x, y) && interior[static_cast<size_t>(y) * grid.width + x] != 0;
  };

  // Find the lowest-then-leftmost interior cell as the start.
  int sx = -1, sy = -1;
  for (int y = 0; y < grid.height && sy < 0; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      if (isInterior(x, y)) {
        sx = x;
        sy = y;
        break;
      }
    }
  }
  std::vector<Vec2d> loop;
  if (sx < 0) return loop;

  // Walk the boundary clockwise in cell space using the standard 8-direction
  // Moore tracing, which handles diagonal connections and thin features that
  // simple 4-connected tracing drops.
  static constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

  int cx = sx, cy = sy;
  int direction = 0;
  const size_t max_steps = static_cast<size_t>(grid.width) * grid.height * 4 + 16;
  for (size_t step = 0; step < max_steps; ++step) {
    loop.push_back(grid.cellCentre(cx, cy));
    // Start searching from the direction that turns left relative to the way we
    // arrived, which keeps the wall on the same side throughout.
    bool moved = false;
    for (int k = 0; k < 8; ++k) {
      const int d = (direction + 6 + k) % 8;
      const int nx = cx + kDx[d], ny = cy + kDy[d];
      if (!isInterior(nx, ny)) continue;
      cx = nx;
      cy = ny;
      direction = d;
      moved = true;
      break;
    }
    if (!moved) break;  // isolated cell
    if (cx == sx && cy == sy) break;
  }
  return loop;
}

/// Dominant edge directions, used to square up the traced outline.
std::vector<double> dominantDirections(const std::vector<Vec2d>& polygon) {
  // Histogram edge angles modulo 90 degrees, weighted by length: walls in a
  // room are overwhelmingly parallel or perpendicular to one another.
  constexpr int kBins = 90;
  std::array<double, kBins> histogram{};
  histogram.fill(0.0);
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Vec2d edge = polygon[(i + 1) % polygon.size()] - polygon[i];
    const double length = edge.norm();
    if (length < 1e-9) continue;
    double angle = std::atan2(edge.y(), edge.x());
    // Fold into [0, pi/2).
    angle = std::fmod(angle + 2.0 * kPi, kPi * 0.5);
    const int bin = std::min(kBins - 1, static_cast<int>(angle / (kPi * 0.5) * kBins));
    histogram[bin] += length;
  }
  int best = 0;
  for (int i = 1; i < kBins; ++i) {
    if (histogram[i] > histogram[best]) best = i;
  }
  const double base = (best + 0.5) / kBins * (kPi * 0.5);
  // The dominant direction and its perpendicular.
  return {base, base + kPi * 0.5, base + kPi, base + kPi * 1.5};
}

double perpendicularDistance(const Vec2d& p, const Vec2d& a, const Vec2d& b) {
  const Vec2d ab = b - a;
  const double length_sq = ab.squaredNorm();
  if (length_sq < 1e-18) return (p - a).norm();
  const double t = std::max(0.0, std::min(1.0, (p - a).dot(ab) / length_sq));
  return (p - (a + ab * t)).norm();
}

void douglasPeucker(const std::vector<Vec2d>& points, size_t first, size_t last,
                    double tolerance, std::vector<uint8_t>* keep) {
  if (last <= first + 1) return;
  double worst = 0.0;
  size_t worst_index = first;
  for (size_t i = first + 1; i < last; ++i) {
    const double d = perpendicularDistance(points[i], points[first], points[last]);
    if (d > worst) {
      worst = d;
      worst_index = i;
    }
  }
  if (worst <= tolerance) return;
  (*keep)[worst_index] = 1;
  douglasPeucker(points, first, worst_index, tolerance, keep);
  douglasPeucker(points, worst_index, last, tolerance, keep);
}




}  // namespace

// ------------------------------------------------------------ polygon maths ---

double polygonSignedArea(const std::vector<Vec2d>& polygon) {
  if (polygon.size() < 3) return 0.0;
  // Shoelace formula.
  double sum = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Vec2d& a = polygon[i];
    const Vec2d& b = polygon[(i + 1) % polygon.size()];
    sum += a.x() * b.y() - b.x() * a.y();
  }
  return sum * 0.5;
}

double polygonPerimeter(const std::vector<Vec2d>& polygon) {
  if (polygon.size() < 2) return 0.0;
  double total = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    total += (polygon[(i + 1) % polygon.size()] - polygon[i]).norm();
  }
  return total;
}

bool pointInPolygon(const std::vector<Vec2d>& polygon, const Vec2d& point) {
  if (polygon.size() < 3) return false;
  // Crossing number: count how many edges a ray to +x passes through.
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const Vec2d& a = polygon[i];
    const Vec2d& b = polygon[j];
    if ((a.y() > point.y()) == (b.y() > point.y())) continue;
    const double x = a.x() + (point.y() - a.y()) / (b.y() - a.y()) * (b.x() - a.x());
    if (point.x() < x) inside = !inside;
  }
  return inside;
}

std::vector<Vec2d> simplifyPolyline(const std::vector<Vec2d>& points, double tolerance,
                                    bool closed) {
  if (points.size() < 3 || tolerance <= 0.0) return points;

  std::vector<uint8_t> keep(points.size(), 0);
  keep.front() = 1;
  keep.back() = 1;
  if (closed) {
    // A closed loop has no natural endpoints, so split it at the two points
    // furthest apart; splitting arbitrarily can shave a whole corner off.
    size_t far_a = 0, far_b = 0;
    double best = -1.0;
    for (size_t i = 0; i < points.size(); ++i) {
      const double d = (points[i] - points[0]).squaredNorm();
      if (d > best) {
        best = d;
        far_a = i;
      }
    }
    best = -1.0;
    for (size_t i = 0; i < points.size(); ++i) {
      const double d = (points[i] - points[far_a]).squaredNorm();
      if (d > best) {
        best = d;
        far_b = i;
      }
    }
    if (far_a > far_b) std::swap(far_a, far_b);
    keep[far_a] = 1;
    keep[far_b] = 1;
    douglasPeucker(points, far_a, far_b, tolerance, &keep);
    // The wrap-around span, handled by rotating a copy.
    std::vector<Vec2d> wrapped;
    std::vector<size_t> mapping;
    for (size_t i = far_b; i < points.size(); ++i) {
      wrapped.push_back(points[i]);
      mapping.push_back(i);
    }
    for (size_t i = 0; i <= far_a; ++i) {
      wrapped.push_back(points[i]);
      mapping.push_back(i);
    }
    std::vector<uint8_t> wrapped_keep(wrapped.size(), 0);
    wrapped_keep.front() = 1;
    wrapped_keep.back() = 1;
    douglasPeucker(wrapped, 0, wrapped.size() - 1, tolerance, &wrapped_keep);
    for (size_t i = 0; i < wrapped.size(); ++i) {
      if (wrapped_keep[i]) keep[mapping[i]] = 1;
    }
  } else {
    douglasPeucker(points, 0, points.size() - 1, tolerance, &keep);
  }

  std::vector<Vec2d> out;
  for (size_t i = 0; i < points.size(); ++i) {
    if (keep[i]) out.push_back(points[i]);
  }
  return out;
}

// --------------------------------------------------------------- generation ---

FloorPlan generateFloorPlan(const std::vector<Vec3f>& points, const std::vector<Vec3f>& normals,
                            const std::vector<DetectedPlane>& provided_planes,
                            const FloorPlanOptions& options) {
  FloorPlan plan;
  if (points.size() < 100) return plan;

  std::vector<DetectedPlane> planes = provided_planes;
  if (planes.empty()) planes = detectPlanes(points, normals, options.planes);

  const Vec3 up = options.planes.up.normalized();

  // ---- locate the floor ---------------------------------------------------
  const DetectedPlane* floor = nullptr;
  const DetectedPlane* ceiling = nullptr;
  for (const DetectedPlane& p : planes) {
    if (p.role == PlaneRole::Floor) floor = &p;
    if (p.role == PlaneRole::Ceiling) ceiling = &p;
  }

  double floor_height = 0.0;
  if (floor != nullptr) {
    Vec3 normal = floor->plane.normal;
    if (normal.dot(up) < 0.0) normal = -normal;
    floor_height = floor->centroid.dot(up);
  } else {
    // No plane was fitted; fall back to a low percentile of the height
    // distribution, which is robust to a few stray points below the floor.
    std::vector<double> heights;
    heights.reserve(points.size());
    for (const Vec3f& v : points) heights.push_back(toDouble(v).dot(up));
    std::sort(heights.begin(), heights.end());
    floor_height = heights[heights.size() / 100];
  }
  plan.floor_height = floor_height;
  if (ceiling != nullptr) {
    plan.ceiling_height = ceiling->centroid.dot(up);
    plan.room_height = plan.ceiling_height - floor_height;
  }

  // ---- build the floor-plane 2D frame ------------------------------------
  Vec3 x_axis = Vec3::UnitX();
  if (std::abs(up.x()) > 0.9) x_axis = Vec3::UnitY();
  x_axis = (x_axis - up * up.dot(x_axis)).normalized();
  const Vec3 y_axis = up.cross(x_axis).normalized();
  plan.origin = up * floor_height;
  plan.x_axis = x_axis;
  plan.y_axis = y_axis;

  const auto toPlane = [&](const Vec3& world) {
    const Vec3 d = world - plan.origin;
    return Vec2d(d.dot(x_axis), d.dot(y_axis));
  };

  // ---- occupancy grid over the wall band ---------------------------------
  Vec2d lo = Vec2d::Constant(1e300), hi = Vec2d::Constant(-1e300);
  std::vector<Vec2d> wall_points;
  std::vector<double> wall_heights;
  std::vector<Vec2d> floor_points;
  // Vertical profile of the vertical surfaces only, at every height rather than
  // just the wall band. Openings are found from this, because a window has
  // wall *below* it and so is invisible in any 2D projection.
  std::vector<Vec2d> profile_xy;
  std::vector<double> profile_height;
  wall_points.reserve(points.size() / 4);
  profile_xy.reserve(points.size() / 2);

  const bool have_normals = normals.size() == points.size();
  const double sin_vertical = std::sin(deg2rad(options.planes.vertical_tolerance_deg));

  for (size_t i = 0; i < points.size(); ++i) {
    const Vec3 world = toDouble(points[i]);
    const double height = world.dot(up) - floor_height;
    const Vec2d p = toPlane(world);
    lo = lo.cwiseMin(p);
    hi = hi.cwiseMax(p);
    if (height >= options.wall_band_low && height <= options.wall_band_high) {
      wall_points.push_back(p);
      wall_heights.push_back(height);
    }
    // Points close to the floor plane define the walkable region.
    if (std::abs(height) <= options.planes.distance_threshold * 2.0) {
      floor_points.push_back(p);
    }
    // Only near-vertical surfaces belong in the wall profile. Including the
    // floor and ceiling would mark the bottom and top height bins occupied
    // everywhere, and a doorway -- whose gap reaches the floor -- would then
    // never register.
    const bool vertical =
        !have_normals || std::abs(toDouble(normals[i]).dot(up)) <= sin_vertical;
    if (vertical && height >= -0.05) {
      profile_xy.push_back(p);
      profile_height.push_back(height);
    }
  }
  if (wall_points.size() < 50 || !(hi.x() > lo.x()) || !(hi.y() > lo.y())) return plan;

  Grid grid;
  grid.resolution = std::max(0.01, options.grid_resolution);
  // Pad by two cells so the flood fill always has an outside to start from.
  grid.origin = lo - Vec2d::Constant(grid.resolution * 2.0);
  grid.width = static_cast<int>(std::ceil((hi.x() - lo.x()) / grid.resolution)) + 5;
  grid.height = static_cast<int>(std::ceil((hi.y() - lo.y()) / grid.resolution)) + 5;
  if (grid.width < 4 || grid.height < 4) return plan;
  // Refuse an absurd grid rather than attempting the allocation.
  if (static_cast<double>(grid.width) * grid.height > 4e7) return plan;
  grid.counts.assign(static_cast<size_t>(grid.width) * grid.height, 0);

  for (const Vec2d& p : wall_points) {
    int x = 0, y = 0;
    grid.toCell(p, &x, &y);
    grid.add(x, y);
  }

  // ---- interior region ----------------------------------------------------
  //
  // The interior comes from the *floor*, not from the walls enclosing it.
  //
  // Deriving it by flood-filling inwards until walls block the fill is the
  // tempting approach, and it is brittle: a single missing cell anywhere in the
  // wall ring lets the fill escape and the entire plan is lost. Real scans have
  // such gaps constantly -- an open doorway to a hallway *is* a gap in a wall,
  // and so is any corner the capture missed. The floor, by contrast, is the one
  // surface a room scan always covers well, because it is what the user walks
  // on and points the phone across.
  std::vector<uint8_t> interior(static_cast<size_t>(grid.width) * grid.height, 0);
  for (const Vec2d& p : floor_points) {
    int x = 0, y = 0;
    grid.toCell(p, &x, &y);
    if (grid.inside(x, y)) interior[static_cast<size_t>(y) * grid.width + x] = 1;
  }

  // Morphological closing bridges the one-cell gaps that sampling leaves when
  // the point spacing is close to the grid resolution, without growing the
  // region overall.
  const auto dilate = [&grid](std::vector<uint8_t>* mask, int radius) {
    std::vector<uint8_t> out(mask->size(), 0);
    for (int y = 0; y < grid.height; ++y) {
      for (int x = 0; x < grid.width; ++x) {
        bool any = false;
        for (int dy = -radius; dy <= radius && !any; ++dy) {
          for (int dx = -radius; dx <= radius; ++dx) {
            if (!grid.inside(x + dx, y + dy)) continue;
            if ((*mask)[static_cast<size_t>(y + dy) * grid.width + (x + dx)]) {
              any = true;
              break;
            }
          }
        }
        out[static_cast<size_t>(y) * grid.width + x] = any ? 1 : 0;
      }
    }
    *mask = std::move(out);
  };
  const auto erode = [&grid](std::vector<uint8_t>* mask, int radius) {
    std::vector<uint8_t> out(mask->size(), 0);
    for (int y = 0; y < grid.height; ++y) {
      for (int x = 0; x < grid.width; ++x) {
        bool all = true;
        for (int dy = -radius; dy <= radius && all; ++dy) {
          for (int dx = -radius; dx <= radius; ++dx) {
            if (!grid.inside(x + dx, y + dy) ||
                !(*mask)[static_cast<size_t>(y + dy) * grid.width + (x + dx)]) {
              all = false;
              break;
            }
          }
        }
        out[static_cast<size_t>(y) * grid.width + x] = all ? 1 : 0;
      }
    }
    *mask = std::move(out);
  };
  dilate(&interior, 1);
  erode(&interior, 1);

  // Fill interior holes: anything not floor and not reachable from the border
  // is furniture or an occluded patch, which is still part of the room's floor
  // area. This is where the flood fill genuinely belongs -- on the complement,
  // where a leak is harmless.
  {
    std::vector<uint8_t> reachable(interior.size(), 0);
    std::queue<std::pair<int, int>> frontier;
    const auto push = [&](int x, int y) {
      if (!grid.inside(x, y)) return;
      const size_t index = static_cast<size_t>(y) * grid.width + x;
      if (reachable[index] || interior[index]) return;
      reachable[index] = 1;
      frontier.emplace(x, y);
    };
    for (int x = 0; x < grid.width; ++x) {
      push(x, 0);
      push(x, grid.height - 1);
    }
    for (int y = 0; y < grid.height; ++y) {
      push(0, y);
      push(grid.width - 1, y);
    }
    while (!frontier.empty()) {
      const auto [x, y] = frontier.front();
      frontier.pop();
      push(x + 1, y);
      push(x - 1, y);
      push(x, y + 1);
      push(x, y - 1);
    }
    for (size_t i = 0; i < interior.size(); ++i) {
      if (!reachable[i]) interior[i] = 1;
    }
  }

  // Keep only the largest connected region, so a stray patch of floor seen
  // through a doorway does not drag the outline into the next room.
  {
    std::vector<int> label(interior.size(), -1);
    int best_label = -1;
    size_t best_size = 0;
    int next_label = 0;
    for (int y0 = 0; y0 < grid.height; ++y0) {
      for (int x0 = 0; x0 < grid.width; ++x0) {
        const size_t seed = static_cast<size_t>(y0) * grid.width + x0;
        if (!interior[seed] || label[seed] >= 0) continue;
        const int current = next_label++;
        size_t size = 0;
        std::queue<std::pair<int, int>> frontier;
        label[seed] = current;
        frontier.emplace(x0, y0);
        while (!frontier.empty()) {
          const auto [x, y] = frontier.front();
          frontier.pop();
          ++size;
          for (const auto& [dx, dy] : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (!grid.inside(nx, ny)) continue;
            const size_t index = static_cast<size_t>(ny) * grid.width + nx;
            if (!interior[index] || label[index] >= 0) continue;
            label[index] = current;
            frontier.emplace(nx, ny);
          }
        }
        if (size > best_size) {
          best_size = size;
          best_label = current;
        }
      }
    }
    if (best_label >= 0) {
      for (size_t i = 0; i < interior.size(); ++i) {
        if (label[i] != best_label) interior[i] = 0;
      }
    }
  }

  // ---- trace and simplify the outline ------------------------------------
  std::vector<Vec2d> traced = traceBoundary(grid, interior);
  if (traced.size() < 4) return plan;
  std::vector<Vec2d> outline = simplifyPolyline(traced, options.simplify_tolerance, true);
  if (outline.size() < 3) return plan;

  // Regularise the outline: snap each edge to a dominant wall direction, push
  // it outward by half a grid cell, and rebuild the vertices as intersections
  // of consecutive edge lines.
  //
  // Doing all three together, on *lines* rather than on points, is what keeps
  // the polygon intact. Rotating each edge about its own midpoint -- the
  // obvious implementation -- moves the two endpoints it shares with its
  // neighbours, so each vertex is written twice with different values and the
  // outline tears itself apart. Intersecting lines cannot do that: every vertex
  // is defined exactly once, by the two edges that meet there.
  //
  // The half-cell outward offset corrects a real bias: wall cells are excluded
  // from the interior, so the traced boundary sits one cell inside the wall and
  // under-reports the floor area by roughly the perimeter times the resolution.
  if (outline.size() >= 3) {
    const std::vector<double> directions =
        options.snap_angle_deg > 0.0 ? dominantDirections(outline) : std::vector<double>{};
    const double snap = deg2rad(options.snap_angle_deg);
    const size_t count = outline.size();

    // Winding first: the outward normal depends on it.
    const bool counter_clockwise = polygonSignedArea(outline) > 0.0;

    struct Line {
      Vec2d point;
      Vec2d direction;
      bool valid = false;
    };
    std::vector<Line> lines(count);
    for (size_t i = 0; i < count; ++i) {
      const Vec2d a = outline[i];
      const Vec2d b = outline[(i + 1) % count];
      Vec2d edge = b - a;
      const double length = edge.norm();
      if (length < 1e-9) continue;
      edge /= length;

      double angle = std::atan2(edge.y(), edge.x());
      for (double target : directions) {
        const double delta = std::fmod(target - angle + kPi * 3.0, kPi * 2.0) - kPi;
        if (std::abs(delta) <= snap) {
          angle = target;
          break;
        }
      }
      const Vec2d direction(std::cos(angle), std::sin(angle));
      // Outward normal: for a counter-clockwise polygon the edge rotated by
      // -90 degrees points away from the interior.
      Vec2d normal(direction.y(), -direction.x());
      if (!counter_clockwise) normal = -normal;

      lines[i].direction = direction;
      lines[i].point = (a + b) * 0.5 + normal * (grid.resolution * 0.5);
      lines[i].valid = true;
    }

    std::vector<Vec2d> rebuilt;
    rebuilt.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      // Vertex i is where the edge arriving at it meets the edge leaving it.
      const Line& incoming = lines[(i + count - 1) % count];
      const Line& outgoing = lines[i];
      if (!incoming.valid || !outgoing.valid) {
        rebuilt.push_back(outline[i]);
        continue;
      }
      // Solve incoming.point + s * incoming.direction == outgoing.point + t * outgoing.direction.
      const double cross = incoming.direction.x() * outgoing.direction.y() -
                           incoming.direction.y() * outgoing.direction.x();
      if (std::abs(cross) < 1e-6) {
        // Nearly collinear: no well-defined corner, so keep what we had.
        rebuilt.push_back(outline[i]);
        continue;
      }
      const Vec2d delta = outgoing.point - incoming.point;
      const double s =
          (delta.x() * outgoing.direction.y() - delta.y() * outgoing.direction.x()) / cross;
      const Vec2d intersection = incoming.point + incoming.direction * s;
      // Reject an intersection that flew far away, which happens when two
      // almost-parallel edges meet at a very shallow angle.
      if (!intersection.allFinite() ||
          (intersection - outline[i]).norm() > grid.resolution * 20.0) {
        rebuilt.push_back(outline[i]);
        continue;
      }
      rebuilt.push_back(intersection);
    }
    // Drop vertices that the regularisation collapsed onto their neighbour.
    std::vector<Vec2d> deduped;
    for (size_t i = 0; i < rebuilt.size(); ++i) {
      if (!deduped.empty() &&
          (rebuilt[i] - deduped.back()).norm() < grid.resolution * 0.25) {
        continue;
      }
      deduped.push_back(rebuilt[i]);
    }
    if (deduped.size() >= 3 &&
        (deduped.front() - deduped.back()).norm() < grid.resolution * 0.25) {
      deduped.pop_back();
    }
    // Drop vertices whose two edges are nearly collinear. Simplification works
    // on perpendicular distance, so a point a few millimetres off a long
    // straight wall survives it; as a corner of the finished plan it is noise,
    // and it would be labelled as two wall segments instead of one.
    if (deduped.size() > 3) {
      bool changed = true;
      while (changed && deduped.size() > 3) {
        changed = false;
        for (size_t i = 0; i < deduped.size(); ++i) {
          const size_t n = deduped.size();
          const Vec2d prev = deduped[(i + n - 1) % n];
          const Vec2d here = deduped[i];
          const Vec2d next = deduped[(i + 1) % n];
          const Vec2d a = (here - prev);
          const Vec2d b = (next - here);
          if (a.norm() < 1e-9 || b.norm() < 1e-9) continue;
          const double turn = std::abs(std::atan2(a.x() * b.y() - a.y() * b.x(), a.dot(b)));
          if (turn < deg2rad(8.0)) {
            deduped.erase(deduped.begin() + static_cast<long>(i));
            changed = true;
            break;
          }
        }
      }
    }
    if (deduped.size() >= 3 && std::abs(polygonSignedArea(deduped)) >= options.min_area) {
      outline = std::move(deduped);
    }
  }

  // Ensure counter-clockwise winding so the area is positive and the wall
  // normals computed below point outwards.
  if (polygonSignedArea(outline) < 0.0) std::reverse(outline.begin(), outline.end());

  plan.outline = outline;
  plan.floor_area = std::abs(polygonSignedArea(outline));
  plan.perimeter = polygonPerimeter(outline);
  if (plan.floor_area < options.min_area) {
    plan.outline.clear();
    plan.floor_area = 0.0;
    return plan;
  }

  // ---- wall segments -----------------------------------------------------
  const double wall_height = plan.room_height > 0.0 ? plan.room_height : options.wall_band_high;
  for (size_t i = 0; i < outline.size(); ++i) {
    WallSegment wall;
    wall.start = outline[i];
    wall.end = outline[(i + 1) % outline.size()];
    const Vec2d edge = wall.end - wall.start;
    if (edge.norm() < 1e-9) continue;
    // For a counter-clockwise polygon the outward normal is the edge rotated
    // by -90 degrees.
    wall.normal = Vec2d(edge.y(), -edge.x()).normalized();
    wall.height = wall_height;
    plan.walls.push_back(wall);
  }

  // ---- openings ----------------------------------------------------------
  //
  // For each wall, bin the nearby vertical-surface points by height along the
  // wall, then look for a contiguous run of empty height bins. A run that
  // reaches the floor is something you can walk through; one that starts higher
  // up is a window. This is why the profile is built in 3D: a 2D occupancy grid
  // cannot tell a window from solid wall, because there is wall beneath it.
  const double wall_top = plan.room_height > 0.0 ? plan.room_height : options.wall_band_high;
  const double bin_height = 0.1;
  const int height_bins = std::max(6, static_cast<int>(std::ceil(wall_top / bin_height)));
  const double min_opening_height = 0.4;
  const double lateral_tolerance = std::max(0.12, grid.resolution * 2.5);

  for (size_t w = 0; w < plan.walls.size(); ++w) {
    const WallSegment& wall = plan.walls[w];
    const double length = wall.length();
    if (length < options.min_opening_width) continue;
    const Vec2d direction = (wall.end - wall.start) / length;

    const int samples =
        std::max(4, static_cast<int>(length / std::max(0.05, grid.resolution)));
    // occupancy[sample * height_bins + bin]
    std::vector<uint8_t> occupancy(static_cast<size_t>(samples) * height_bins, 0);
    int samples_with_any_wall = 0;

    for (size_t i = 0; i < profile_xy.size(); ++i) {
      const Vec2d d = profile_xy[i] - wall.start;
      const double along = d.dot(direction);
      if (along < 0.0 || along > length) continue;
      if (std::abs(d.dot(wall.normal)) > lateral_tolerance) continue;
      const int s = std::min(samples - 1, static_cast<int>(along / length * samples));
      const int b = static_cast<int>(profile_height[i] / bin_height);
      if (b < 0 || b >= height_bins) continue;
      occupancy[static_cast<size_t>(s) * height_bins + b] = 1;
    }
    for (int s = 0; s < samples; ++s) {
      for (int b = 0; b < height_bins; ++b) {
        if (occupancy[static_cast<size_t>(s) * height_bins + b]) {
          ++samples_with_any_wall;
          break;
        }
      }
    }
    // If this edge has almost no wall behind it, it is an open side of the
    // scan rather than a wall with openings in it.
    if (samples_with_any_wall * 4 < samples) continue;

    // Largest empty height run per sample.
    std::vector<int> gap_low(samples, -1), gap_high(samples, -1);
    for (int s = 0; s < samples; ++s) {
      int best_start = -1, best_length = 0, run_start = -1;
      for (int b = 0; b <= height_bins; ++b) {
        const bool empty =
            b < height_bins && !occupancy[static_cast<size_t>(s) * height_bins + b];
        if (empty && run_start < 0) {
          run_start = b;
        } else if (!empty && run_start >= 0) {
          if (b - run_start > best_length) {
            best_length = b - run_start;
            best_start = run_start;
          }
          run_start = -1;
        }
      }
      if (best_length * bin_height >= min_opening_height) {
        gap_low[s] = best_start;
        gap_high[s] = best_start + best_length;
      }
    }

    // Group adjacent samples whose gaps overlap into a single opening.
    int run_start = -1;
    int run_low = 0, run_high = 0;
    const auto flush = [&](int run_end) {
      if (run_start < 0) return;
      const double width = length * (run_end - run_start) / samples;
      if (width >= options.min_opening_width) {
        Opening opening;
        opening.wall_index = static_cast<int>(w);
        opening.start = wall.start + direction * (length * run_start / samples);
        opening.end = wall.start + direction * (length * run_end / samples);
        opening.sill_height = run_low * bin_height;
        opening.head_height = std::min(wall_top, run_high * bin_height);
        if (opening.head_height <= opening.sill_height) {
          opening.head_height = opening.sill_height + min_opening_height;
        }
        if (opening.sill_height <= options.door_sill_threshold) {
          opening.kind = width > 1.6 ? OpeningKind::Passage : OpeningKind::Door;
        } else {
          opening.kind = OpeningKind::Window;
        }
        plan.openings.push_back(opening);
      }
      run_start = -1;
    };

    for (int s = 0; s <= samples; ++s) {
      const bool has_gap = s < samples && gap_low[s] >= 0;
      if (!has_gap) {
        flush(s);
        continue;
      }
      if (run_start < 0) {
        run_start = s;
        run_low = gap_low[s];
        run_high = gap_high[s];
        continue;
      }
      // Require the gaps to overlap, so a door and a window on the same wall
      // are not merged into one opening spanning both.
      const int overlap = std::min(run_high, gap_high[s]) - std::max(run_low, gap_low[s]);
      if (overlap <= 0) {
        flush(s);
        run_start = s;
        run_low = gap_low[s];
        run_high = gap_high[s];
        continue;
      }
      run_low = std::max(run_low, gap_low[s]);
      run_high = std::min(run_high, gap_high[s]);
    }
  }

  return plan;
}

FloorPlan generateFloorPlan(const Mesh& mesh, const std::vector<DetectedPlane>& planes,
                            const FloorPlanOptions& options) {
  return generateFloorPlan(mesh.vertices, mesh.normals, planes, options);
}

}  // namespace parallax
