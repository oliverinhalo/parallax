// Floor plan extraction from a room scan.
//
// Approach: find the floor plane, project the geometry between knee height and
// head height onto a 2D occupancy grid, and trace the room outline from it.
//
// The obvious alternative -- intersect the detected wall planes pairwise and
// order the results around the centroid -- only works for convex rooms, and
// fails on the L-shaped and alcoved rooms that are extremely common. An
// occupancy grid handles arbitrary floor shapes, degrades gracefully when a
// wall was missed, and gives openings for free as gaps in wall coverage.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/exporters/MeshExport.h"  // LengthUnit
#include "parallax/floorplan/PlaneDetection.h"
#include "parallax/mesh/Mesh.h"

#include <string>

namespace parallax {

/// A 2D point in the floor plane, in metres.
using Vec2d = Vec2;

struct WallSegment {
  Vec2d start = Vec2d::Zero();
  Vec2d end = Vec2d::Zero();
  double length() const { return (end - start).norm(); }
  /// Outward normal in the floor plane (pointing away from the room interior).
  Vec2d normal = Vec2d::Zero();
  double height = 0.0;
};

enum class OpeningKind { Door, Window, Passage };

struct Opening {
  int wall_index = -1;
  Vec2d start = Vec2d::Zero();
  Vec2d end = Vec2d::Zero();
  double sill_height = 0.0;    ///< bottom of the opening above the floor
  double head_height = 0.0;    ///< top of the opening above the floor
  OpeningKind kind = OpeningKind::Passage;
  double width() const { return (end - start).norm(); }
  double height() const { return head_height - sill_height; }
};

struct FloorPlan {
  /// Outline of the floor, counter-clockwise, in metres. The first point is not
  /// repeated at the end.
  std::vector<Vec2d> outline;
  std::vector<WallSegment> walls;
  std::vector<Opening> openings;

  double floor_height = 0.0;    ///< world height of the floor plane
  double ceiling_height = 0.0;  ///< world height of the ceiling, if found
  double room_height = 0.0;     ///< ceiling minus floor, 0 if no ceiling
  double floor_area = 0.0;      ///< m^2, from the outline polygon
  double perimeter = 0.0;       ///< m
  /// Transform from the floor-plane 2D frame back into world coordinates:
  /// world = origin + x_axis * p.x() + y_axis * p.y().
  Vec3 origin = Vec3::Zero();
  Vec3 x_axis = Vec3::UnitX();
  Vec3 y_axis = Vec3::UnitZ();

  bool valid() const { return outline.size() >= 3 && floor_area > 0.0; }
  Vec3 toWorld(const Vec2d& p) const { return origin + x_axis * p.x() + y_axis * p.y(); }
};

struct FloorPlanOptions {
  /// Occupancy grid resolution in metres. 5 cm resolves a doorway reliably
  /// without making the outline noisy.
  double grid_resolution = 0.05;
  /// Height band above the floor used to detect walls. Starting above the floor
  /// avoids skirting boards and clutter; stopping below the ceiling avoids
  /// coving and light fittings.
  double wall_band_low = 0.4;
  double wall_band_high = 1.8;
  /// Cells with at least this many points count as occupied.
  int min_points_per_cell = 2;
  /// Douglas-Peucker tolerance for simplifying the traced outline.
  double simplify_tolerance = 0.06;
  /// Snap outline edges to the dominant wall directions within this angle.
  double snap_angle_deg = 12.0;
  /// Ignore outline loops enclosing less than this area (clutter, not rooms).
  double min_area = 0.8;
  /// Openings narrower than this are noise, not doors.
  double min_opening_width = 0.5;
  /// A gap whose sill is below this is a door or passage, not a window.
  double door_sill_threshold = 0.25;
  PlaneDetectionOptions planes;
};

/**
 * Build a floor plan from a reconstructed room.
 *
 * @param mesh    the reconstructed room mesh (vertices and normals are used)
 * @param planes  planes already detected, or empty to detect them here
 */
FloorPlan generateFloorPlan(const Mesh& mesh, const std::vector<DetectedPlane>& planes = {},
                            const FloorPlanOptions& options = {});

FloorPlan generateFloorPlan(const std::vector<Vec3f>& points, const std::vector<Vec3f>& normals,
                            const std::vector<DetectedPlane>& planes = {},
                            const FloorPlanOptions& options = {});

/// Signed area of a polygon (positive for counter-clockwise).
double polygonSignedArea(const std::vector<Vec2d>& polygon);
double polygonPerimeter(const std::vector<Vec2d>& polygon);
bool pointInPolygon(const std::vector<Vec2d>& polygon, const Vec2d& point);

/// Douglas-Peucker simplification of an open or closed polyline.
std::vector<Vec2d> simplifyPolyline(const std::vector<Vec2d>& points, double tolerance,
                                    bool closed = true);

struct FloorPlanStyle {
  double pixels_per_metre = 100.0;
  double margin_px = 48.0;
  bool show_dimensions = true;
  bool show_area = true;
  bool show_openings = true;
  bool dark = false;
  std::string title;
  LengthUnit unit = LengthUnit::Metres;
};

/// Render the plan as an SVG document.
std::string renderFloorPlanSvg(const FloorPlan& plan, const FloorPlanStyle& style = {});

/// Render the plan as a raster image (used for project thumbnails and PNG export).
ColorBuffer renderFloorPlanRaster(const FloorPlan& plan, const FloorPlanStyle& style = {});

/**
 * Render the plan as a single-page PDF.
 *
 * A minimal, self-contained PDF 1.4 writer: the plan is lines and text, which
 * needs only the base-14 Helvetica font and the path operators, so there is no
 * reason to take on a PDF library for it.
 */
std::vector<uint8_t> renderFloorPlanPdf(const FloorPlan& plan,
                                        const FloorPlanStyle& style = {});

}  // namespace parallax
