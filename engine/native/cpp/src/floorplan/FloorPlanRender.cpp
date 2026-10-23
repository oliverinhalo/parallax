// Floor plan rendering: SVG, raster and PDF.
#include "parallax/floorplan/FloorPlan.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace parallax {
namespace {

struct Palette {
  std::string background;
  std::string wall;
  std::string floor;
  std::string opening;
  std::string door;
  std::string text;
  std::string dimension;
  std::string accent;
};

Palette palette(bool dark) {
  if (dark) {
    // Matches the app's dark theme: graphite surfaces, electric cyan accent.
    return Palette{"#121417", "#E8EDF2", "#1C2024", "#3DE0FF", "#FFB454", "#E8EDF2",
                   "#8A939B", "#3DE0FF"};
  }
  return Palette{"#FFFFFF", "#15181C", "#F2F4F6", "#0096B8", "#C2700A", "#15181C",
                 "#5E6670", "#0096B8"};
}

/// Layout mapping from plan metres to output pixels, with Y flipped so the plan
/// reads the way a person expects (north up).
struct Layout {
  double scale = 100.0;
  double margin = 48.0;
  Vec2d min = Vec2d::Zero();
  double width = 0.0;
  double height = 0.0;

  Vec2d toPixels(const Vec2d& p) const {
    return Vec2d(margin + (p.x() - min.x()) * scale,
                 height - margin - (p.y() - min.y()) * scale);
  }
};

Layout computeLayout(const FloorPlan& plan, const FloorPlanStyle& style) {
  Layout layout;
  layout.scale = std::max(1.0, style.pixels_per_metre);
  layout.margin = std::max(0.0, style.margin_px);

  Vec2d lo = Vec2d::Constant(1e300), hi = Vec2d::Constant(-1e300);
  for (const Vec2d& p : plan.outline) {
    lo = lo.cwiseMin(p);
    hi = hi.cwiseMax(p);
  }
  if (!(hi.x() > lo.x())) return layout;
  layout.min = lo;
  layout.width = (hi.x() - lo.x()) * layout.scale + layout.margin * 2.0;
  layout.height = (hi.y() - lo.y()) * layout.scale + layout.margin * 2.0;
  // Leave room at the bottom for the area caption.
  if (style.show_area) layout.height += 34.0;
  return layout;
}

std::string fmt(double v, int precision = 2) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

std::string escapeXml(const std::string& in) {
  std::string out;
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string formatLengthLocal(double metres, LengthUnit unit) {
  const double value = metres * unitScaleFromMetres(unit);
  std::ostringstream oss;
  if (unit == LengthUnit::Millimetres || unit == LengthUnit::Centimetres) {
    oss << std::fixed << std::setprecision(0) << value;
  } else {
    oss << std::fixed << std::setprecision(2) << value;
  }
  return oss.str() + " " + unitName(unit);
}

/// Bresenham line into a raster buffer, with a given thickness.
void drawLine(ColorBuffer* image, Vec2d a, Vec2d b, const std::array<uint8_t, 3>& color,
              int thickness) {
  const int steps = static_cast<int>(std::ceil((b - a).norm())) + 1;
  const int radius = std::max(0, thickness / 2);
  for (int s = 0; s <= steps; ++s) {
    const double t = steps > 0 ? static_cast<double>(s) / steps : 0.0;
    const Vec2d p = a + (b - a) * t;
    const int px = static_cast<int>(std::lround(p.x()));
    const int py = static_cast<int>(std::lround(p.y()));
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        const int x = px + dx, y = py + dy;
        if (x < 0 || y < 0 || x >= image->width || y >= image->height) continue;
        image->set(x, y, color[0], color[1], color[2]);
      }
    }
  }
}

std::array<uint8_t, 3> hexToRgb(const std::string& hex) {
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  if (hex.size() != 7 || hex[0] != '#') return {0, 0, 0};
  return {static_cast<uint8_t>(digit(hex[1]) * 16 + digit(hex[2])),
          static_cast<uint8_t>(digit(hex[3]) * 16 + digit(hex[4])),
          static_cast<uint8_t>(digit(hex[5]) * 16 + digit(hex[6]))};
}

}  // namespace

std::string renderFloorPlanSvg(const FloorPlan& plan, const FloorPlanStyle& style) {
  const Palette colors = palette(style.dark);
  std::ostringstream svg;

  if (!plan.valid()) {
    // Still emit a well-formed document, so a caller writing it to disk does
    // not end up with a zero-byte "file".
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"120\" "
           "viewBox=\"0 0 320 120\">\n";
    svg << "  <rect width=\"320\" height=\"120\" fill=\"" << colors.background << "\"/>\n";
    svg << "  <text x=\"160\" y=\"64\" text-anchor=\"middle\" font-family=\"sans-serif\" "
           "font-size=\"14\" fill=\"" << colors.dimension
        << "\">No floor plan available</text>\n";
    svg << "</svg>\n";
    return svg.str();
  }

  const Layout layout = computeLayout(plan, style);

  svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(layout.width, 1)
      << "\" height=\"" << fmt(layout.height, 1) << "\" viewBox=\"0 0 "
      << fmt(layout.width, 1) << " " << fmt(layout.height, 1) << "\">\n";
  svg << "  <rect width=\"100%\" height=\"100%\" fill=\"" << colors.background << "\"/>\n";

  // Floor fill.
  svg << "  <path d=\"";
  for (size_t i = 0; i < plan.outline.size(); ++i) {
    const Vec2d p = layout.toPixels(plan.outline[i]);
    svg << (i == 0 ? "M " : "L ") << fmt(p.x(), 1) << " " << fmt(p.y(), 1) << " ";
  }
  svg << "Z\" fill=\"" << colors.floor << "\" stroke=\"none\"/>\n";

  // Walls.
  svg << "  <g stroke=\"" << colors.wall
      << "\" stroke-width=\"5\" stroke-linecap=\"square\" stroke-linejoin=\"miter\">\n";
  for (const WallSegment& wall : plan.walls) {
    const Vec2d a = layout.toPixels(wall.start);
    const Vec2d b = layout.toPixels(wall.end);
    svg << "    <line x1=\"" << fmt(a.x(), 1) << "\" y1=\"" << fmt(a.y(), 1) << "\" x2=\""
        << fmt(b.x(), 1) << "\" y2=\"" << fmt(b.y(), 1) << "\"/>\n";
  }
  svg << "  </g>\n";

  // Openings drawn over the wall, so a doorway reads as a break in it.
  if (style.show_openings) {
    for (const Opening& opening : plan.openings) {
      const Vec2d a = layout.toPixels(opening.start);
      const Vec2d b = layout.toPixels(opening.end);
      const std::string color =
          opening.kind == OpeningKind::Window ? colors.opening : colors.door;
      svg << "  <line x1=\"" << fmt(a.x(), 1) << "\" y1=\"" << fmt(a.y(), 1) << "\" x2=\""
          << fmt(b.x(), 1) << "\" y2=\"" << fmt(b.y(), 1) << "\" stroke=\"" << colors.floor
          << "\" stroke-width=\"7\" stroke-linecap=\"butt\"/>\n";
      svg << "  <line x1=\"" << fmt(a.x(), 1) << "\" y1=\"" << fmt(a.y(), 1) << "\" x2=\""
          << fmt(b.x(), 1) << "\" y2=\"" << fmt(b.y(), 1) << "\" stroke=\"" << color
          << "\" stroke-width=\"2.5\" stroke-linecap=\"butt\"/>\n";
    }
  }

  // Wall length labels, placed just outside each wall.
  if (style.show_dimensions) {
    svg << "  <g font-family=\"ui-monospace, SFMono-Regular, Menlo, monospace\" "
           "font-size=\"11\" fill=\"" << colors.dimension << "\">\n";
    for (const WallSegment& wall : plan.walls) {
      if (wall.length() < 0.35) continue;  // too short to label legibly
      const Vec2d mid = (wall.start + wall.end) * 0.5;
      const Vec2d p = layout.toPixels(mid + wall.normal * 0.22);
      double angle = std::atan2(-(wall.end.y() - wall.start.y()),
                                wall.end.x() - wall.start.x());
      // Keep text upright rather than upside down on the far walls.
      if (angle > kPi * 0.5 || angle < -kPi * 0.5) angle += kPi;
      svg << "    <text x=\"" << fmt(p.x(), 1) << "\" y=\"" << fmt(p.y(), 1)
          << "\" text-anchor=\"middle\" transform=\"rotate(" << fmt(rad2deg(angle), 1) << " "
          << fmt(p.x(), 1) << " " << fmt(p.y(), 1) << ")\">"
          << escapeXml(formatLengthLocal(wall.length(), style.unit)) << "</text>\n";
    }
    svg << "  </g>\n";
  }

  // Caption: title, area and room height.
  if (style.show_area) {
    std::ostringstream caption;
    if (!style.title.empty()) caption << style.title << "  \xc2\xb7  ";
    const double scale = unitScaleFromMetres(style.unit);
    caption << fmt(plan.floor_area * scale * scale) << " " << unitName(style.unit)
            << "\xc2\xb2";
    if (plan.room_height > 0.0) {
      caption << "  \xc2\xb7  ceiling " << formatLengthLocal(plan.room_height, style.unit);
    }
    svg << "  <text x=\"" << fmt(layout.margin, 1) << "\" y=\""
        << fmt(layout.height - 12.0, 1)
        << "\" font-family=\"ui-sans-serif, system-ui, sans-serif\" font-size=\"13\" fill=\""
        << colors.text << "\">" << escapeXml(caption.str()) << "</text>\n";
  }

  svg << "</svg>\n";
  return svg.str();
}

ColorBuffer renderFloorPlanRaster(const FloorPlan& plan, const FloorPlanStyle& style) {
  const Palette colors = palette(style.dark);
  const Layout layout = computeLayout(plan, style);
  const int width = std::max(16, static_cast<int>(std::ceil(layout.width)));
  const int height = std::max(16, static_cast<int>(std::ceil(layout.height)));

  ColorBuffer image(width, height);
  const std::array<uint8_t, 3> background = hexToRgb(colors.background);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      image.set(x, y, background[0], background[1], background[2]);
    }
  }
  if (!plan.valid()) return image;

  // Fill the interior by scanline, using the polygon test directly: the plan is
  // small enough that this is cheaper than building an edge table.
  const std::array<uint8_t, 3> floor = hexToRgb(colors.floor);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // Convert back to plan coordinates.
      const Vec2d p(layout.min.x() + (x - layout.margin) / layout.scale,
                    layout.min.y() + (layout.height - layout.margin - y) / layout.scale);
      if (pointInPolygon(plan.outline, p)) image.set(x, y, floor[0], floor[1], floor[2]);
    }
  }

  const std::array<uint8_t, 3> wall_color = hexToRgb(colors.wall);
  for (const WallSegment& wall : plan.walls) {
    drawLine(&image, layout.toPixels(wall.start), layout.toPixels(wall.end), wall_color, 5);
  }
  if (style.show_openings) {
    for (const Opening& opening : plan.openings) {
      const std::array<uint8_t, 3> color =
          hexToRgb(opening.kind == OpeningKind::Window ? colors.opening : colors.door);
      drawLine(&image, layout.toPixels(opening.start), layout.toPixels(opening.end), floor, 7);
      drawLine(&image, layout.toPixels(opening.start), layout.toPixels(opening.end), color, 3);
    }
  }
  return image;
}

std::vector<uint8_t> renderFloorPlanPdf(const FloorPlan& plan, const FloorPlanStyle& style) {
  // A minimal single-page PDF 1.4. The plan is paths and text in Helvetica, one
  // of the base-14 fonts every reader has built in, so no font embedding and no
  // PDF library are needed.
  const Layout layout = computeLayout(plan, style);
  const double page_width = std::max(144.0, layout.width);
  const double page_height = std::max(144.0, layout.height);

  std::ostringstream content;
  content << std::fixed << std::setprecision(2);
  // PDF's origin is bottom-left with Y up, the opposite of the layout, so flip.
  const auto pdfPoint = [&layout, page_height](const Vec2d& p) {
    const Vec2d px = layout.toPixels(p);
    return Vec2d(px.x(), page_height - px.y());
  };

  if (plan.valid()) {
    // Floor fill in light grey.
    content << "0.95 0.96 0.97 rg\n";
    for (size_t i = 0; i < plan.outline.size(); ++i) {
      const Vec2d p = pdfPoint(plan.outline[i]);
      content << p.x() << " " << p.y() << (i == 0 ? " m\n" : " l\n");
    }
    content << "h f\n";

    // Walls: 2.4pt black strokes.
    content << "0 0 0 RG\n2.4 w\n1 J\n1 j\n";
    for (const WallSegment& wall : plan.walls) {
      const Vec2d a = pdfPoint(wall.start);
      const Vec2d b = pdfPoint(wall.end);
      content << a.x() << " " << a.y() << " m " << b.x() << " " << b.y() << " l S\n";
    }

    if (style.show_openings) {
      content << "0.0 0.59 0.72 RG\n1.6 w\n";
      for (const Opening& opening : plan.openings) {
        const Vec2d a = pdfPoint(opening.start);
        const Vec2d b = pdfPoint(opening.end);
        // Erase the wall under the opening, then draw it in the accent colour.
        content << "1 1 1 RG 3.4 w " << a.x() << " " << a.y() << " m " << b.x() << " "
                << b.y() << " l S\n";
        content << "0.0 0.59 0.72 RG 1.6 w " << a.x() << " " << a.y() << " m " << b.x() << " "
                << b.y() << " l S\n";
      }
    }

    if (style.show_dimensions) {
      content << "BT /F1 8 Tf 0.35 0.4 0.44 rg\n";
      for (const WallSegment& wall : plan.walls) {
        if (wall.length() < 0.35) continue;
        const Vec2d p = pdfPoint((wall.start + wall.end) * 0.5 + wall.normal * 0.22);
        // PDF text strings need parentheses and backslashes escaped.
        std::string label = formatLengthLocal(wall.length(), style.unit);
        std::string escaped;
        for (char c : label) {
          if (c == '(' || c == ')' || c == '\\') escaped.push_back('\\');
          escaped.push_back(c);
        }
        content << "1 0 0 1 " << p.x() << " " << p.y() << " Tm (" << escaped << ") Tj\n";
      }
      content << "ET\n";
    }

    if (style.show_area) {
      const double scale = unitScaleFromMetres(style.unit);
      std::ostringstream caption;
      if (!style.title.empty()) caption << style.title << " - ";
      caption << std::fixed << std::setprecision(2) << plan.floor_area * scale * scale << " "
              << unitName(style.unit) << "2";
      content << "BT /F1 11 Tf 0 0 0 rg 1 0 0 1 " << layout.margin << " " << 16.0 << " Tm ("
              << caption.str() << ") Tj ET\n";
    }
  } else {
    content << "BT /F1 12 Tf 0.4 0.4 0.4 rg 1 0 0 1 24 " << (page_height / 2)
            << " Tm (No floor plan available) Tj ET\n";
  }

  const std::string stream = content.str();
  std::ostringstream pdf;
  std::vector<size_t> offsets;
  const auto startObject = [&pdf, &offsets](int number) {
    offsets.push_back(static_cast<size_t>(pdf.tellp()));
    pdf << number << " 0 obj\n";
  };

  pdf << "%PDF-1.4\n";
  // A binary comment marks the file as containing binary data, which keeps
  // naive tools from mangling it.
  pdf << "%\xE2\xE3\xCF\xD3\n";

  startObject(1);
  pdf << "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
  startObject(2);
  pdf << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
  startObject(3);
  pdf << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << std::fixed << std::setprecision(2)
      << page_width << " " << page_height
      << "] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n";
  startObject(4);
  pdf << "<< /Length " << stream.size() << " >>\nstream\n" << stream << "\nendstream\nendobj\n";
  startObject(5);
  pdf << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding "
         ">>\nendobj\n";

  const size_t xref_offset = static_cast<size_t>(pdf.tellp());
  pdf << "xref\n0 " << (offsets.size() + 1) << "\n";
  pdf << "0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << std::setfill(' ');
  pdf << "trailer\n<< /Size " << (offsets.size() + 1) << " /Root 1 0 R >>\n";
  pdf << "startxref\n" << xref_offset << "\n%%EOF\n";

  const std::string text = pdf.str();
  return std::vector<uint8_t>(text.begin(), text.end());
}

}  // namespace parallax
