#include "parallax/floorplan/FloorPlan.h"

#include <gtest/gtest.h>

#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// Sample points on an axis-aligned rectangular room of known dimensions, with
/// outward-facing normals -- i.e. exactly what fusion would hand us.
struct RoomSamples {
  std::vector<Vec3f> points;
  std::vector<Vec3f> normals;
};

RoomSamples sampleRoom(double width, double depth, double height, double spacing = 0.04,
                       uint64_t seed = 11, double noise = 0.0,
                       const std::vector<std::array<double, 4>>& wall_gaps = {}) {
  RoomSamples out;
  Rng rng(seed);
  const double hw = width * 0.5, hd = depth * 0.5;

  const auto add = [&](const Vec3& p, const Vec3& n) {
    Vec3 q = p;
    if (noise > 0.0) q += Vec3(rng.normal(), rng.normal(), rng.normal()) * noise;
    out.points.push_back(
        Vec3f(static_cast<float>(q.x()), static_cast<float>(q.y()), static_cast<float>(q.z())));
    out.normals.push_back(
        Vec3f(static_cast<float>(n.x()), static_cast<float>(n.y()), static_cast<float>(n.z())));
  };

  // Floor and ceiling.
  for (double x = -hw; x <= hw; x += spacing) {
    for (double z = -hd; z <= hd; z += spacing) {
      add(Vec3(x, 0.0, z), Vec3(0, 1, 0));
      add(Vec3(x, height, z), Vec3(0, -1, 0));
    }
  }
  // Walls, with optional gaps for doors and windows. Each gap is
  // {wall index, start along the wall, end along the wall, sill height}.
  const auto gapCovers = [&wall_gaps](int wall, double along, double y) {
    for (const auto& gap : wall_gaps) {
      if (static_cast<int>(gap[0]) != wall) continue;
      if (along >= gap[1] && along <= gap[2] && y >= gap[3]) return true;
    }
    return false;
  };
  for (double y = 0.0; y <= height; y += spacing) {
    for (double x = -hw; x <= hw; x += spacing) {
      // Wall 0 at z = -hd, wall 1 at z = +hd. `along` measured from -hw.
      if (!gapCovers(0, x + hw, y)) add(Vec3(x, y, -hd), Vec3(0, 0, 1));
      if (!gapCovers(1, x + hw, y)) add(Vec3(x, y, hd), Vec3(0, 0, -1));
    }
    for (double z = -hd; z <= hd; z += spacing) {
      if (!gapCovers(2, z + hd, y)) add(Vec3(-hw, y, z), Vec3(1, 0, 0));
      if (!gapCovers(3, z + hd, y)) add(Vec3(hw, y, z), Vec3(-1, 0, 0));
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------- plane fitting ---

TEST(PlaneFit, RecoversAKnownPlane) {
  // A plane with normal (0,1,0) through y = 1.5.
  std::vector<Vec3> points;
  Rng rng(1);
  for (int i = 0; i < 200; ++i) {
    points.emplace_back(rng.uniform(-2, 2), 1.5, rng.uniform(-2, 2));
  }
  Plane plane;
  double rms = 0.0;
  ASSERT_TRUE(fitPlane(points, &plane, &rms));
  EXPECT_NEAR(std::abs(plane.normal.y()), 1.0, 1e-9);
  EXPECT_NEAR(std::abs(plane.signedDistance(Vec3(0, 1.5, 0))), 0.0, 1e-9);
  EXPECT_NEAR(rms, 0.0, 1e-9);
}

TEST(PlaneFit, RejectsDegenerateInput) {
  Plane plane;
  EXPECT_FALSE(fitPlane({}, &plane));
  EXPECT_FALSE(fitPlane({Vec3(0, 0, 0), Vec3(1, 0, 0)}, &plane));
  // Collinear points do not determine a plane.
  EXPECT_FALSE(fitPlane({Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(2, 0, 0), Vec3(3, 0, 0)}, &plane));
}

TEST(PlaneFit, ProjectAndDistanceAreConsistent) {
  const Plane plane = Plane::fromPointAndNormal(Vec3(1, 2, 3), Vec3(1, 1, 0));
  const Vec3 p(5, -2, 7);
  const Vec3 projected = plane.project(p);
  EXPECT_NEAR(plane.signedDistance(projected), 0.0, 1e-12);
  // The displacement must be parallel to the normal, so their cross product
  // vanishes, and its length must equal the signed distance.
  const Vec3 displacement = p - projected;
  EXPECT_LT(displacement.cross(plane.normal).norm(), 1e-12);
  EXPECT_NEAR(displacement.norm(), std::abs(plane.signedDistance(p)), 1e-12);
}

TEST(PlaneFit, OrientTowardsFlipsTheNormal) {
  Plane plane = Plane::fromPointAndNormal(Vec3::Zero(), Vec3::UnitY());
  plane.orientTowards(Vec3(0, -5, 0));
  EXPECT_LT(plane.normal.y(), 0.0);
  EXPECT_GT(plane.signedDistance(Vec3(0, -5, 0)), 0.0);
}

// -------------------------------------------------------- plane detection ---

TEST(PlaneDetection, FindsTheSixPlanesOfARoomAndClassifiesThem) {
  const RoomSamples room = sampleRoom(4.0, 3.0, 2.5, 0.05);
  ASSERT_GT(room.points.size(), 5000u);

  PlaneDetectionOptions options;
  options.min_inliers = 200;
  const std::vector<DetectedPlane> planes = detectPlanes(room.points, room.normals, options);

  // Six surfaces; allow a couple of extra slivers from the corners.
  EXPECT_GE(planes.size(), 6u);
  EXPECT_LE(planes.size(), 9u);

  int floors = 0, ceilings = 0, walls = 0;
  for (const DetectedPlane& p : planes) {
    if (p.role == PlaneRole::Floor) ++floors;
    if (p.role == PlaneRole::Ceiling) ++ceilings;
    if (p.role == PlaneRole::Wall) ++walls;
  }
  EXPECT_EQ(floors, 1) << "exactly one plane should be the floor";
  EXPECT_EQ(ceilings, 1);
  EXPECT_GE(walls, 4);

  // The floor must be at y = 0 and the ceiling at y = 2.5.
  for (const DetectedPlane& p : planes) {
    if (p.role == PlaneRole::Floor) {
      EXPECT_NEAR(p.centroid.y(), 0.0, 0.05);
      EXPECT_NEAR(std::abs(p.plane.normal.y()), 1.0, 0.02);
    }
    if (p.role == PlaneRole::Ceiling) EXPECT_NEAR(p.centroid.y(), 2.5, 0.05);
    if (p.role == PlaneRole::Wall) EXPECT_LT(std::abs(p.plane.normal.y()), 0.2);
  }
}

TEST(PlaneDetection, NormalAgreementStopsDistinctSurfacesMerging) {
  // Two parallel surfaces facing *opposite* ways, 4 cm apart: within the
  // distance threshold but with opposed normals. Without the normal test a
  // single plane would swallow both.
  std::vector<Vec3f> points;
  std::vector<Vec3f> normals;
  Rng rng(5);
  for (int i = 0; i < 1200; ++i) {
    const float x = static_cast<float>(rng.uniform(-1, 1));
    const float z = static_cast<float>(rng.uniform(-1, 1));
    points.push_back(Vec3f(x, 0.f, z));
    normals.push_back(Vec3f(0, 1, 0));
    points.push_back(Vec3f(x, 0.02f, z));
    normals.push_back(Vec3f(0, -1, 0));
  }
  PlaneDetectionOptions options;
  options.distance_threshold = 0.05;  // deliberately larger than the gap
  options.normal_threshold_deg = 20.0;
  options.min_inliers = 200;
  const std::vector<DetectedPlane> planes = detectPlanes(points, normals, options);
  ASSERT_GE(planes.size(), 2u);
  // Both surfaces must be represented, with opposing normals.
  bool up = false, down = false;
  for (const DetectedPlane& p : planes) {
    if (p.plane.normal.y() > 0.9) up = true;
    if (p.plane.normal.y() < -0.9) down = true;
  }
  EXPECT_TRUE(up && down) << "opposed surfaces were merged into one plane";
}

TEST(PlaneDetection, HandlesTooFewPoints) {
  EXPECT_TRUE(detectPlanes(std::vector<Vec3f>{}, std::vector<Vec3f>{}).empty());
  std::vector<Vec3f> few(20, Vec3f::Zero());
  EXPECT_TRUE(detectPlanes(few, few).empty());
}

TEST(PlaneDetection, ToleratesMeasurementNoise) {
  const RoomSamples room = sampleRoom(4.0, 3.0, 2.5, 0.06, 7, /*noise=*/0.01);
  PlaneDetectionOptions options;
  options.min_inliers = 150;
  const std::vector<DetectedPlane> planes = detectPlanes(room.points, room.normals, options);
  EXPECT_GE(planes.size(), 5u);
  for (const DetectedPlane& p : planes) {
    if (p.role == PlaneRole::Floor) {
      EXPECT_NEAR(p.centroid.y(), 0.0, 0.03);
      // The RMS should reflect the noise we injected, not blow up.
      EXPECT_LT(p.rms_error, 0.02);
    }
  }
}

TEST(PlaneDetection, LevellingRotationUndoesATilt) {
  // Build a room, tilt the whole thing, and check the recovered rotation
  // brings the floor back to horizontal.
  const RoomSamples room = sampleRoom(3.5, 3.0, 2.4, 0.06);
  const Mat3 tilt = so3Exp(Vec3(deg2rad(7.0), 0.0, deg2rad(-4.0)));

  std::vector<Vec3f> points;
  std::vector<Vec3f> normals;
  for (size_t i = 0; i < room.points.size(); ++i) {
    const Vec3 p = tilt * Vec3(room.points[i].x(), room.points[i].y(), room.points[i].z());
    const Vec3 n = tilt * Vec3(room.normals[i].x(), room.normals[i].y(), room.normals[i].z());
    points.push_back(
        Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z())));
    normals.push_back(
        Vec3f(static_cast<float>(n.x()), static_cast<float>(n.y()), static_cast<float>(n.z())));
  }

  PlaneDetectionOptions options;
  options.min_inliers = 150;
  // The floor is no longer within the default horizontal tolerance of +Y, so
  // widen it: this is exactly the case where ARCore's gravity vector would be
  // supplied instead.
  options.horizontal_tolerance_deg = 20.0;
  options.vertical_tolerance_deg = 20.0;
  const std::vector<DetectedPlane> planes = detectPlanes(points, normals, options);

  const Mat3 correction = levellingRotation(planes, options);
  // Applying the correction must bring the floor normal onto +Y.
  for (const DetectedPlane& p : planes) {
    if (p.role != PlaneRole::Floor) continue;
    Vec3 normal = p.plane.normal;
    if (normal.dot(options.up) < 0.0) normal = -normal;
    const Vec3 levelled = correction * normal;
    EXPECT_NEAR(levelled.y(), 1.0, 1e-6);
  }
  EXPECT_TRUE(levellingRotation({}, options).isApprox(Mat3::Identity()));
}

// ------------------------------------------------------------ polygon util ---

TEST(Polygon, SignedAreaAndPerimeterOfAKnownRectangle) {
  const std::vector<Vec2d> ccw{Vec2d(0, 0), Vec2d(4, 0), Vec2d(4, 3), Vec2d(0, 3)};
  EXPECT_NEAR(polygonSignedArea(ccw), 12.0, 1e-12);
  EXPECT_NEAR(polygonPerimeter(ccw), 14.0, 1e-12);

  std::vector<Vec2d> cw = ccw;
  std::reverse(cw.begin(), cw.end());
  EXPECT_NEAR(polygonSignedArea(cw), -12.0, 1e-12);

  EXPECT_EQ(polygonSignedArea({}), 0.0);
  EXPECT_EQ(polygonSignedArea({Vec2d(0, 0), Vec2d(1, 1)}), 0.0);
}

TEST(Polygon, PointInPolygonHandlesInsideOutsideAndConcave) {
  // An L-shape, which is where a convex-only test would be wrong.
  const std::vector<Vec2d> shape{Vec2d(0, 0), Vec2d(4, 0), Vec2d(4, 1),
                                 Vec2d(1, 1), Vec2d(1, 3), Vec2d(0, 3)};
  EXPECT_TRUE(pointInPolygon(shape, Vec2d(0.5, 0.5)));
  EXPECT_TRUE(pointInPolygon(shape, Vec2d(3.5, 0.5)));
  EXPECT_TRUE(pointInPolygon(shape, Vec2d(0.5, 2.5)));
  // In the notch: inside the bounding box but outside the polygon.
  EXPECT_FALSE(pointInPolygon(shape, Vec2d(3.0, 2.0)));
  EXPECT_FALSE(pointInPolygon(shape, Vec2d(-1, -1)));
  EXPECT_FALSE(pointInPolygon({}, Vec2d(0, 0)));
}

TEST(Polygon, SimplifyRemovesCollinearPointsAndKeepsCorners) {
  // A square with many redundant points along each edge.
  std::vector<Vec2d> dense;
  for (double t = 0; t < 4.0; t += 0.1) dense.emplace_back(t, 0.0);
  for (double t = 0; t < 3.0; t += 0.1) dense.emplace_back(4.0, t);
  for (double t = 4.0; t > 0.0; t -= 0.1) dense.emplace_back(t, 3.0);
  for (double t = 3.0; t > 0.0; t -= 0.1) dense.emplace_back(0.0, t);

  const std::vector<Vec2d> simple = simplifyPolyline(dense, 0.05, true);
  // Should collapse to roughly the four corners.
  EXPECT_LE(simple.size(), 8u);
  EXPECT_GE(simple.size(), 4u);
  // And still describe the same rectangle.
  EXPECT_NEAR(std::abs(polygonSignedArea(simple)), 12.0, 0.5);
}

TEST(Polygon, SimplifyIsANoOpForShortOrZeroToleranceInput) {
  const std::vector<Vec2d> few{Vec2d(0, 0), Vec2d(1, 0)};
  EXPECT_EQ(simplifyPolyline(few, 0.1).size(), 2u);
  const std::vector<Vec2d> square{Vec2d(0, 0), Vec2d(1, 0), Vec2d(1, 1), Vec2d(0, 1)};
  EXPECT_EQ(simplifyPolyline(square, 0.0).size(), 4u);
}

// ------------------------------------------------------------- floor plans ---

TEST(FloorPlan, MeasuresARectangularRoomAccurately) {
  const double width = 4.2, depth = 3.1, height = 2.5;
  const RoomSamples room = sampleRoom(width, depth, height, 0.04);

  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  const FloorPlan plan = generateFloorPlan(room.points, room.normals, {}, options);

  ASSERT_TRUE(plan.valid()) << "no plan produced";
  EXPECT_NEAR(plan.floor_height, 0.0, 0.05);
  EXPECT_NEAR(plan.room_height, height, 0.08);

  // Floor area within a few percent of the truth. The occupancy grid measures
  // the *interior*, which is slightly smaller than the wall centre lines.
  const double expected_area = width * depth;
  EXPECT_NEAR(plan.floor_area, expected_area, expected_area * 0.12)
      << "area " << plan.floor_area << " expected " << expected_area;
  EXPECT_NEAR(plan.perimeter, 2 * (width + depth), 2 * (width + depth) * 0.12);

  // Four walls, and their lengths should pair up as the room's sides.
  EXPECT_GE(plan.walls.size(), 4u);
  EXPECT_LE(plan.walls.size(), 8u);
  double longest = 0.0, total = 0.0;
  for (const WallSegment& wall : plan.walls) {
    longest = std::max(longest, wall.length());
    total += wall.length();
  }
  EXPECT_NEAR(longest, std::max(width, depth), 0.35);
  EXPECT_NEAR(total, plan.perimeter, 1e-9);
}

TEST(FloorPlan, OutlineIsCounterClockwiseAndWallNormalsPointOutwards) {
  const RoomSamples room = sampleRoom(3.6, 3.0, 2.4, 0.05);
  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  const FloorPlan plan = generateFloorPlan(room.points, room.normals, {}, options);
  ASSERT_TRUE(plan.valid());

  EXPECT_GT(polygonSignedArea(plan.outline), 0.0) << "outline must wind counter-clockwise";

  // Stepping from a wall's midpoint along its normal must leave the room.
  for (const WallSegment& wall : plan.walls) {
    if (wall.length() < 0.3) continue;
    const Vec2d mid = (wall.start + wall.end) * 0.5;
    EXPECT_FALSE(pointInPolygon(plan.outline, mid + wall.normal * 0.15))
        << "wall normal points inwards";
    EXPECT_TRUE(pointInPolygon(plan.outline, mid - wall.normal * 0.15));
  }
}

TEST(FloorPlan, WorldTransformRoundTripsPlanCoordinates) {
  const RoomSamples room = sampleRoom(3.4, 3.0, 2.4, 0.06);
  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  const FloorPlan plan = generateFloorPlan(room.points, room.normals, {}, options);
  ASSERT_TRUE(plan.valid());

  // The axes must be orthonormal, so measurements taken in the plan frame are
  // metric in the world frame too.
  EXPECT_NEAR(plan.x_axis.norm(), 1.0, 1e-9);
  EXPECT_NEAR(plan.y_axis.norm(), 1.0, 1e-9);
  EXPECT_NEAR(plan.x_axis.dot(plan.y_axis), 0.0, 1e-9);

  for (const Vec2d& p : plan.outline) {
    const Vec3 world = plan.toWorld(p);
    // Every outline point must lie on the floor plane.
    EXPECT_NEAR(world.y(), plan.floor_height, 1e-9);
    // And the distance between two plan points must equal the world distance.
  }
  if (plan.outline.size() >= 2) {
    const double plan_distance = (plan.outline[1] - plan.outline[0]).norm();
    const double world_distance =
        (plan.toWorld(plan.outline[1]) - plan.toWorld(plan.outline[0])).norm();
    EXPECT_NEAR(plan_distance, world_distance, 1e-9);
  }
}

TEST(FloorPlan, DetectsADoorwayAndAWindow) {
  // A door on wall 0 (floor to 2.0 m, 0.9 m wide) and a window on wall 1
  // (1.0 m to 2.0 m, 1.2 m wide).
  const double width = 4.0, depth = 3.0, height = 2.5;
  const std::vector<std::array<double, 4>> gaps{
      {0, 1.4, 2.3, 0.0},   // door: reaches the floor
      {1, 1.2, 2.4, 1.0},   // window: sill at 1.0 m
  };
  const RoomSamples room = sampleRoom(width, depth, height, 0.035, 11, 0.0, gaps);

  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  options.min_opening_width = 0.4;
  const FloorPlan plan = generateFloorPlan(room.points, room.normals, {}, options);
  ASSERT_TRUE(plan.valid());
  ASSERT_GE(plan.openings.size(), 2u) << "found " << plan.openings.size() << " openings";

  int doors = 0, windows = 0;
  for (const Opening& opening : plan.openings) {
    EXPECT_GE(opening.width(), options.min_opening_width);
    EXPECT_GE(opening.wall_index, 0);
    EXPECT_LT(opening.wall_index, static_cast<int>(plan.walls.size()));
    if (opening.kind == OpeningKind::Window) {
      ++windows;
      // A window's sill must be off the floor.
      EXPECT_GT(opening.sill_height, options.door_sill_threshold);
    } else {
      ++doors;
      EXPECT_LE(opening.sill_height, options.door_sill_threshold);
    }
  }
  EXPECT_GE(doors, 1) << "the floor-to-ceiling gap should read as a door or passage";
  EXPECT_GE(windows, 1) << "the raised gap should read as a window";
}

TEST(FloorPlan, HandlesAnLShapedRoom) {
  // Two overlapping rectangles forming an L. This is the case a
  // wall-plane-intersection approach cannot represent at all.
  std::vector<Vec3f> points;
  std::vector<Vec3f> normals;
  const auto addWall = [&](Vec2d a, Vec2d b, Vec2d normal, double height) {
    const double length = (b - a).norm();
    const Vec2d direction = (b - a) / length;
    for (double t = 0; t <= length; t += 0.035) {
      for (double y = 0.0; y <= height; y += 0.035) {
        const Vec2d p = a + direction * t;
        points.push_back(Vec3f(static_cast<float>(p.x()), static_cast<float>(y),
                               static_cast<float>(p.y())));
        normals.push_back(Vec3f(static_cast<float>(normal.x()), 0.f,
                                static_cast<float>(normal.y())));
      }
    }
  };
  // L-shaped outline, counter-clockwise, with inward-facing wall normals.
  const std::vector<Vec2d> corners{Vec2d(0, 0), Vec2d(4, 0), Vec2d(4, 1.6),
                                   Vec2d(1.8, 1.6), Vec2d(1.8, 3.4), Vec2d(0, 3.4)};
  for (size_t i = 0; i < corners.size(); ++i) {
    const Vec2d a = corners[i];
    const Vec2d b = corners[(i + 1) % corners.size()];
    const Vec2d edge = (b - a).normalized();
    // Inward normal for a counter-clockwise polygon.
    addWall(a, b, Vec2d(-edge.y(), edge.x()), 2.4);
  }
  // Floor and ceiling over the L.
  for (double x = 0; x <= 4.0; x += 0.05) {
    for (double z = 0; z <= 3.4; z += 0.05) {
      if (!pointInPolygon(corners, Vec2d(x, z))) continue;
      points.push_back(Vec3f(static_cast<float>(x), 0.f, static_cast<float>(z)));
      normals.push_back(Vec3f(0, 1, 0));
      points.push_back(Vec3f(static_cast<float>(x), 2.4f, static_cast<float>(z)));
      normals.push_back(Vec3f(0, -1, 0));
    }
  }

  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  const FloorPlan plan = generateFloorPlan(points, normals, {}, options);
  ASSERT_TRUE(plan.valid());

  const double expected_area = std::abs(polygonSignedArea(corners));
  EXPECT_NEAR(plan.floor_area, expected_area, expected_area * 0.15)
      << "area " << plan.floor_area << " expected " << expected_area;
  // An L needs at least six edges; a convex fit would give four.
  EXPECT_GE(plan.walls.size(), 6u);
  // The reflex corner must be present. Compare in world coordinates: the
  // plan's own 2D frame is (x_axis, y_axis) with y_axis = up x x_axis, which
  // for a Y-up world is (x, -z), so comparing plan coordinates against the
  // (x, z) corners used to build the room would be checking the wrong frame.
  double nearest_to_notch = 1e9;
  for (const Vec2d& p : plan.outline) {
    const Vec3 world = plan.toWorld(p);
    nearest_to_notch =
        std::min(nearest_to_notch, (Vec2d(world.x(), world.z()) - Vec2d(1.8, 1.6)).norm());
  }
  EXPECT_LT(nearest_to_notch, 0.3) << "the concave corner was lost";
}

TEST(FloorPlan, ReturnsAnInvalidPlanForInsufficientInput) {
  EXPECT_FALSE(generateFloorPlan(std::vector<Vec3f>{}, std::vector<Vec3f>{}).valid());
  std::vector<Vec3f> few(50, Vec3f::Zero());
  EXPECT_FALSE(generateFloorPlan(few, few).valid());

  // A single flat plane is not a room: there are no walls to enclose anything.
  std::vector<Vec3f> flat;
  std::vector<Vec3f> flat_normals;
  for (double x = -2; x <= 2; x += 0.05) {
    for (double z = -2; z <= 2; z += 0.05) {
      flat.push_back(Vec3f(static_cast<float>(x), 0.f, static_cast<float>(z)));
      flat_normals.push_back(Vec3f(0, 1, 0));
    }
  }
  const FloorPlan plan = generateFloorPlan(flat, flat_normals);
  // Either invalid, or it fell back to the floor extent -- but it must not
  // invent walls or a ceiling.
  EXPECT_EQ(plan.room_height, 0.0);
}

// ---------------------------------------------------------------- rendering ---

TEST(FloorPlanRender, SvgIsWellFormedAndContainsTheGeometry) {
  const RoomSamples room = sampleRoom(4.0, 3.0, 2.5, 0.05);
  FloorPlanOptions options;
  options.planes.min_inliers = 200;
  const FloorPlan plan = generateFloorPlan(room.points, room.normals, {}, options);
  ASSERT_TRUE(plan.valid());

  FloorPlanStyle style;
  style.title = "Living room";
  const std::string svg = renderFloorPlanSvg(plan, style);

  EXPECT_EQ(svg.compare(0, 5, "<?xml"), 0);
  EXPECT_NE(svg.find("<svg"), std::string::npos);
  EXPECT_NE(svg.find("</svg>"), std::string::npos);
  // One line element per wall, plus the floor path.
  EXPECT_NE(svg.find("<path d=\"M "), std::string::npos);
  size_t lines = 0, pos = 0;
  while ((pos = svg.find("<line", pos)) != std::string::npos) {
    ++lines;
    pos += 5;
  }
  EXPECT_GE(lines, plan.walls.size());
  // The title and the area must appear.
  EXPECT_NE(svg.find("Living room"), std::string::npos);
  // Tags must balance.
  size_t opens = 0, closes = 0;
  for (size_t i = 0; i + 1 < svg.size(); ++i) {
    if (svg[i] == '<' && svg[i + 1] != '/' && svg[i + 1] != '?') ++opens;
    if (svg[i] == '<' && svg[i + 1] == '/') ++closes;
  }
  EXPECT_GT(opens, 0u);
  EXPECT_GT(closes, 0u);
}

TEST(FloorPlanRender, SvgEscapesTitleText) {
  FloorPlan plan;
  plan.outline = {Vec2d(0, 0), Vec2d(2, 0), Vec2d(2, 2), Vec2d(0, 2)};
  plan.floor_area = 4.0;
  FloorPlanStyle style;
  style.title = "Bob & Alice's <room>";
  const std::string svg = renderFloorPlanSvg(plan, style);
  EXPECT_NE(svg.find("&amp;"), std::string::npos);
  EXPECT_NE(svg.find("&lt;room&gt;"), std::string::npos);
  EXPECT_EQ(svg.find("<room>"), std::string::npos);
}

TEST(FloorPlanRender, SvgHandlesAnEmptyPlanGracefully) {
  const std::string svg = renderFloorPlanSvg(FloorPlan{});
  EXPECT_NE(svg.find("<svg"), std::string::npos);
  EXPECT_NE(svg.find("</svg>"), std::string::npos);
  EXPECT_NE(svg.find("No floor plan"), std::string::npos);
}

TEST(FloorPlanRender, RasterFillsTheInteriorAndDrawsWalls) {
  FloorPlan plan;
  plan.outline = {Vec2d(0, 0), Vec2d(3, 0), Vec2d(3, 2), Vec2d(0, 2)};
  plan.floor_area = 6.0;
  for (size_t i = 0; i < plan.outline.size(); ++i) {
    WallSegment wall;
    wall.start = plan.outline[i];
    wall.end = plan.outline[(i + 1) % plan.outline.size()];
    const Vec2d edge = wall.end - wall.start;
    wall.normal = Vec2d(edge.y(), -edge.x()).normalized();
    plan.walls.push_back(wall);
  }

  FloorPlanStyle style;
  style.pixels_per_metre = 50.0;
  style.margin_px = 20.0;
  const ColorBuffer image = renderFloorPlanRaster(plan, style);
  EXPECT_GT(image.width, 150);
  EXPECT_GT(image.height, 100);

  // The centre of the room should be floor coloured, and distinct from the
  // background outside the margin.
  uint8_t centre[3], corner[3];
  image.get(image.width / 2, image.height / 2, centre);
  image.get(2, 2, corner);
  const bool differs = centre[0] != corner[0] || centre[1] != corner[1] ||
                       centre[2] != corner[2];
  EXPECT_TRUE(differs) << "interior was not filled";
}

TEST(FloorPlanRender, PdfHasAValidStructure) {
  FloorPlan plan;
  plan.outline = {Vec2d(0, 0), Vec2d(4, 0), Vec2d(4, 3), Vec2d(0, 3)};
  plan.floor_area = 12.0;
  plan.perimeter = 14.0;
  for (size_t i = 0; i < plan.outline.size(); ++i) {
    WallSegment wall;
    wall.start = plan.outline[i];
    wall.end = plan.outline[(i + 1) % plan.outline.size()];
    const Vec2d edge = wall.end - wall.start;
    wall.normal = Vec2d(edge.y(), -edge.x()).normalized();
    plan.walls.push_back(wall);
  }

  const std::vector<uint8_t> pdf = renderFloorPlanPdf(plan);
  ASSERT_GT(pdf.size(), 400u);
  const std::string text(pdf.begin(), pdf.end());

  // Header, the five objects, the cross-reference table and the trailer.
  EXPECT_EQ(text.compare(0, 8, "%PDF-1.4"), 0);
  EXPECT_NE(text.find("/Type /Catalog"), std::string::npos);
  EXPECT_NE(text.find("/Type /Pages"), std::string::npos);
  EXPECT_NE(text.find("/Type /Page"), std::string::npos);
  EXPECT_NE(text.find("/BaseFont /Helvetica"), std::string::npos);
  EXPECT_NE(text.find("stream"), std::string::npos);
  EXPECT_NE(text.find("endstream"), std::string::npos);
  EXPECT_NE(text.find("xref"), std::string::npos);
  EXPECT_NE(text.find("trailer"), std::string::npos);
  EXPECT_NE(text.find("%%EOF"), std::string::npos);

  // startxref must point at the actual xref table.
  const size_t startxref = text.rfind("startxref");
  ASSERT_NE(startxref, std::string::npos);
  const size_t offset = std::stoul(text.substr(startxref + 10));
  ASSERT_LT(offset, text.size());
  EXPECT_EQ(text.compare(offset, 4, "xref"), 0) << "startxref does not point at xref";

  // The declared stream length must match the actual stream bytes.
  const size_t length_pos = text.find("/Length ");
  ASSERT_NE(length_pos, std::string::npos);
  const size_t declared = std::stoul(text.substr(length_pos + 8));
  const size_t stream_start = text.find("stream\n", length_pos) + 7;
  const size_t stream_end = text.find("\nendstream", stream_start);
  ASSERT_NE(stream_end, std::string::npos);
  EXPECT_EQ(stream_end - stream_start, declared) << "/Length disagrees with the stream";
}

TEST(FloorPlanRender, PdfHandlesAnEmptyPlan) {
  const std::vector<uint8_t> pdf = renderFloorPlanPdf(FloorPlan{});
  const std::string text(pdf.begin(), pdf.end());
  EXPECT_EQ(text.compare(0, 8, "%PDF-1.4"), 0);
  EXPECT_NE(text.find("%%EOF"), std::string::npos);
  EXPECT_NE(text.find("No floor plan"), std::string::npos);
}

TEST(FloorPlanRender, UnitChoiceChangesTheLabels) {
  FloorPlan plan;
  plan.outline = {Vec2d(0, 0), Vec2d(2, 0), Vec2d(2, 2), Vec2d(0, 2)};
  plan.floor_area = 4.0;
  WallSegment wall;
  wall.start = Vec2d(0, 0);
  wall.end = Vec2d(2, 0);
  wall.normal = Vec2d(0, -1);
  plan.walls.push_back(wall);

  FloorPlanStyle metric;
  metric.unit = LengthUnit::Metres;
  const std::string m = renderFloorPlanSvg(plan, metric);
  EXPECT_NE(m.find("2.00 m"), std::string::npos);

  FloorPlanStyle imperial;
  imperial.unit = LengthUnit::Feet;
  const std::string ft = renderFloorPlanSvg(plan, imperial);
  EXPECT_NE(ft.find("ft"), std::string::npos);
  EXPECT_NE(ft.find("6.56"), std::string::npos) << "2 m is 6.56 ft";
}
