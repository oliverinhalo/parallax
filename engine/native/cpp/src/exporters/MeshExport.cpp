#include "parallax/exporters/MeshExport.h"

#include "parallax/io/ImageIo.h"
#include "parallax/io/Json.h"
#include "parallax/io/Zip.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace parallax {
namespace {

template <typename T>
void appendRaw(std::vector<uint8_t>* out, const T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "raw append needs a POD");
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

void padTo(std::vector<uint8_t>* out, size_t alignment, uint8_t filler) {
  while (out->size() % alignment != 0) out->push_back(filler);
}

/// Format a float with enough digits to round-trip, and without an exponent
/// where avoidable -- some OBJ and USDA consumers dislike `1e-05`.
std::string formatFloat(double v, int precision = 6) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  std::string s = oss.str();
  // Trim trailing zeros but keep at least one decimal digit.
  const size_t dot = s.find('.');
  if (dot != std::string::npos) {
    size_t last = s.size() - 1;
    while (last > dot + 1 && s[last] == '0') --last;
    s.erase(last + 1);
  }
  return s;
}

/// Downscale a texture so its longest edge is at most `max_size`.
ColorBuffer fitTexture(const ColorBuffer& texture, int max_size) {
  if (texture.empty() || max_size <= 0) return texture;
  const int longest = std::max(texture.width, texture.height);
  if (longest <= max_size) return texture;

  const double scale = static_cast<double>(max_size) / longest;
  const int w = std::max(1, static_cast<int>(std::lround(texture.width * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(texture.height * scale)));
  ColorBuffer out(w, h);
  // Box filter over the source footprint of each destination pixel: cheap, and
  // it avoids the aliasing that point sampling a 4k atlas down to 1k produces.
  for (int y = 0; y < h; ++y) {
    const int sy0 = static_cast<int>(y * texture.height / h);
    const int sy1 = std::max(sy0 + 1, static_cast<int>((y + 1) * texture.height / h));
    for (int x = 0; x < w; ++x) {
      const int sx0 = static_cast<int>(x * texture.width / w);
      const int sx1 = std::max(sx0 + 1, static_cast<int>((x + 1) * texture.width / w));
      long r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1 && sy < texture.height; ++sy) {
        for (int sx = sx0; sx < sx1 && sx < texture.width; ++sx) {
          uint8_t rgb[3];
          texture.get(sx, sy, rgb);
          r += rgb[0];
          g += rgb[1];
          b += rgb[2];
          ++n;
        }
      }
      if (n == 0) n = 1;
      out.set(x, y, static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n),
              static_cast<uint8_t>(b / n));
    }
  }
  return out;
}

}  // namespace

double unitScaleFromMetres(LengthUnit unit) {
  switch (unit) {
    case LengthUnit::Metres: return 1.0;
    case LengthUnit::Centimetres: return 100.0;
    case LengthUnit::Millimetres: return 1000.0;
    case LengthUnit::Inches: return 1000.0 / 25.4;
    case LengthUnit::Feet: return 1000.0 / 304.8;
  }
  return 1.0;
}

const char* unitName(LengthUnit unit) {
  switch (unit) {
    case LengthUnit::Metres: return "m";
    case LengthUnit::Centimetres: return "cm";
    case LengthUnit::Millimetres: return "mm";
    case LengthUnit::Inches: return "in";
    case LengthUnit::Feet: return "ft";
  }
  return "m";
}

Mat3 exportBasis(const ExportOptions& options) {
  // The engine works in a right-handed, Y-up frame (the ARCore/glTF
  // convention). Everything here is expressed relative to that.
  Mat3 basis = Mat3::Identity();
  if (options.up_axis == UpAxis::ZUp) {
    // Rotate -90 degrees about X: Y -> Z, Z -> -Y.
    basis << 1, 0, 0,
             0, 0, -1,
             0, 1, 0;
  }
  if (options.handedness == Handedness::LeftHanded) {
    // Mirror one axis. This reverses triangle winding, which the exporters
    // compensate for by flipping the index order.
    Mat3 mirror = Mat3::Identity();
    mirror(0, 0) = -1.0;
    basis = mirror * basis;
  }
  return basis;
}

Mesh applyExportTransform(const Mesh& mesh, const ExportOptions& options) {
  Mesh out = mesh;
  if (options.target_triangles > 0 &&
      static_cast<int>(out.triangleCount()) > options.target_triangles) {
    decimateMesh(&out, options.target_triangles);
  }
  const double scale = unitScaleFromMetres(options.unit);
  const Mat3 basis = exportBasis(options);
  transformMesh(&out, basis, Vec3::Zero(), scale);

  // A mirrored basis inverts the winding; swapping two indices per triangle
  // restores outward-facing normals in the destination convention.
  if (basis.determinant() < 0.0) {
    for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
      std::swap(out.indices[t + 1], out.indices[t + 2]);
    }
  }
  if (!options.include_normals) out.normals.clear();
  if (!options.include_vertex_colors) out.colors.clear();
  if (!options.include_uvs) out.uvs.clear();
  return out;
}

// ------------------------------------------------------------------- PLY ---

bool writePly(const std::string& path, const Mesh& mesh, const ExportOptions& options) {
  if (mesh.empty()) return false;
  const Mesh m = applyExportTransform(mesh, options);
  const bool normals = m.hasNormals();
  const bool colors = m.hasColors();

  std::ostringstream header;
  header << "ply\n";
  header << (options.binary ? "format binary_little_endian 1.0\n" : "format ascii 1.0\n");
  header << "comment generated by " << options.generator << "\n";
  header << "comment units " << unitName(options.unit) << "\n";
  header << "element vertex " << m.vertexCount() << "\n";
  header << "property float x\nproperty float y\nproperty float z\n";
  if (normals) header << "property float nx\nproperty float ny\nproperty float nz\n";
  if (colors) header << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  header << "element face " << m.triangleCount() << "\n";
  header << "property list uchar uint vertex_indices\n";
  header << "end_header\n";

  if (!options.binary) {
    std::ostringstream body;
    body << header.str();
    for (size_t i = 0; i < m.vertexCount(); ++i) {
      body << formatFloat(m.vertices[i].x()) << ' ' << formatFloat(m.vertices[i].y()) << ' '
           << formatFloat(m.vertices[i].z());
      if (normals) {
        body << ' ' << formatFloat(m.normals[i].x()) << ' ' << formatFloat(m.normals[i].y())
             << ' ' << formatFloat(m.normals[i].z());
      }
      if (colors) {
        body << ' ' << static_cast<int>(m.colors[i][0]) << ' '
             << static_cast<int>(m.colors[i][1]) << ' ' << static_cast<int>(m.colors[i][2]);
      }
      body << '\n';
    }
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      body << "3 " << m.indices[t] << ' ' << m.indices[t + 1] << ' ' << m.indices[t + 2]
           << '\n';
    }
    return writeFileText(path, body.str());
  }

  std::vector<uint8_t> bytes;
  const std::string header_text = header.str();
  bytes.insert(bytes.end(), header_text.begin(), header_text.end());
  for (size_t i = 0; i < m.vertexCount(); ++i) {
    appendRaw(&bytes, m.vertices[i].x());
    appendRaw(&bytes, m.vertices[i].y());
    appendRaw(&bytes, m.vertices[i].z());
    if (normals) {
      appendRaw(&bytes, m.normals[i].x());
      appendRaw(&bytes, m.normals[i].y());
      appendRaw(&bytes, m.normals[i].z());
    }
    if (colors) {
      bytes.push_back(m.colors[i][0]);
      bytes.push_back(m.colors[i][1]);
      bytes.push_back(m.colors[i][2]);
    }
  }
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    bytes.push_back(3);
    appendRaw(&bytes, m.indices[t]);
    appendRaw(&bytes, m.indices[t + 1]);
    appendRaw(&bytes, m.indices[t + 2]);
  }
  return writeFileBytes(path, bytes);
}

bool writePointCloudPly(const std::string& path, const std::vector<Vec3f>& points,
                        const std::vector<Vec3f>& normals,
                        const std::vector<std::array<uint8_t, 3>>& colors,
                        const ExportOptions& options) {
  if (points.empty()) return false;
  const bool has_normals = normals.size() == points.size();
  const bool has_colors = colors.size() == points.size();
  const double scale = unitScaleFromMetres(options.unit);
  const Mat3 basis = exportBasis(options);

  std::ostringstream header;
  header << "ply\n";
  header << (options.binary ? "format binary_little_endian 1.0\n" : "format ascii 1.0\n");
  header << "comment generated by " << options.generator << "\n";
  header << "comment units " << unitName(options.unit) << "\n";
  header << "element vertex " << points.size() << "\n";
  header << "property float x\nproperty float y\nproperty float z\n";
  if (has_normals) header << "property float nx\nproperty float ny\nproperty float nz\n";
  if (has_colors) header << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  header << "end_header\n";

  const auto transformPoint = [&](const Vec3f& v) {
    const Vec3 p = basis * Vec3(v.x(), v.y(), v.z()) * scale;
    return Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()),
                 static_cast<float>(p.z()));
  };
  const auto transformNormal = [&](const Vec3f& v) {
    const Vec3 n = (basis * Vec3(v.x(), v.y(), v.z())).normalized();
    return Vec3f(static_cast<float>(n.x()), static_cast<float>(n.y()),
                 static_cast<float>(n.z()));
  };

  if (!options.binary) {
    std::ostringstream body;
    body << header.str();
    for (size_t i = 0; i < points.size(); ++i) {
      const Vec3f p = transformPoint(points[i]);
      body << formatFloat(p.x()) << ' ' << formatFloat(p.y()) << ' ' << formatFloat(p.z());
      if (has_normals) {
        const Vec3f n = transformNormal(normals[i]);
        body << ' ' << formatFloat(n.x()) << ' ' << formatFloat(n.y()) << ' '
             << formatFloat(n.z());
      }
      if (has_colors) {
        body << ' ' << static_cast<int>(colors[i][0]) << ' ' << static_cast<int>(colors[i][1])
             << ' ' << static_cast<int>(colors[i][2]);
      }
      body << '\n';
    }
    return writeFileText(path, body.str());
  }

  std::vector<uint8_t> bytes;
  const std::string header_text = header.str();
  bytes.insert(bytes.end(), header_text.begin(), header_text.end());
  for (size_t i = 0; i < points.size(); ++i) {
    const Vec3f p = transformPoint(points[i]);
    appendRaw(&bytes, p.x());
    appendRaw(&bytes, p.y());
    appendRaw(&bytes, p.z());
    if (has_normals) {
      const Vec3f n = transformNormal(normals[i]);
      appendRaw(&bytes, n.x());
      appendRaw(&bytes, n.y());
      appendRaw(&bytes, n.z());
    }
    if (has_colors) {
      bytes.push_back(colors[i][0]);
      bytes.push_back(colors[i][1]);
      bytes.push_back(colors[i][2]);
    }
  }
  return writeFileBytes(path, bytes);
}

// ------------------------------------------------------------------- STL ---

bool writeStl(const std::string& path, const Mesh& mesh, const ExportOptions& options) {
  if (mesh.empty()) return false;
  const Mesh m = applyExportTransform(mesh, options);

  const auto faceNormal = [&m](size_t t) {
    const Vec3 a(m.vertices[m.indices[t]].x(), m.vertices[m.indices[t]].y(),
                 m.vertices[m.indices[t]].z());
    const Vec3 b(m.vertices[m.indices[t + 1]].x(), m.vertices[m.indices[t + 1]].y(),
                 m.vertices[m.indices[t + 1]].z());
    const Vec3 c(m.vertices[m.indices[t + 2]].x(), m.vertices[m.indices[t + 2]].y(),
                 m.vertices[m.indices[t + 2]].z());
    const Vec3 n = (b - a).cross(c - a);
    return n.norm() > 1e-20 ? n.normalized() : Vec3(0, 0, 1);
  };

  if (!options.binary) {
    std::ostringstream body;
    body << "solid " << options.name << "\n";
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      const Vec3 n = faceNormal(t);
      body << "  facet normal " << formatFloat(n.x()) << ' ' << formatFloat(n.y()) << ' '
           << formatFloat(n.z()) << "\n    outer loop\n";
      for (int k = 0; k < 3; ++k) {
        const Vec3f& v = m.vertices[m.indices[t + k]];
        body << "      vertex " << formatFloat(v.x()) << ' ' << formatFloat(v.y()) << ' '
             << formatFloat(v.z()) << '\n';
      }
      body << "    endloop\n  endfacet\n";
    }
    body << "endsolid " << options.name << "\n";
    return writeFileText(path, body.str());
  }

  std::vector<uint8_t> bytes;
  // 80-byte header. STL carries no unit field, so record the unit here: it is
  // the only in-file hint a human will ever get.
  std::string header = "Parallax " + options.name + " units=" + unitName(options.unit);
  header.resize(80, '\0');
  bytes.insert(bytes.end(), header.begin(), header.end());
  appendRaw(&bytes, static_cast<uint32_t>(m.triangleCount()));
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const Vec3 n = faceNormal(t);
    appendRaw(&bytes, static_cast<float>(n.x()));
    appendRaw(&bytes, static_cast<float>(n.y()));
    appendRaw(&bytes, static_cast<float>(n.z()));
    for (int k = 0; k < 3; ++k) {
      const Vec3f& v = m.vertices[m.indices[t + k]];
      appendRaw(&bytes, v.x());
      appendRaw(&bytes, v.y());
      appendRaw(&bytes, v.z());
    }
    appendRaw(&bytes, static_cast<uint16_t>(0));  // attribute byte count
  }
  return writeFileBytes(path, bytes);
}

// ------------------------------------------------------------------- OBJ ---

bool writeObj(const std::string& directory, const std::string& base_name, const Mesh& mesh,
              const ColorBuffer& texture, const ExportOptions& options) {
  if (mesh.empty() || base_name.empty()) return false;
  const Mesh m = applyExportTransform(mesh, options);
  const std::string prefix = directory.empty() ? base_name : directory + "/" + base_name;

  const ColorBuffer fitted = fitTexture(texture, options.max_texture_size);
  const bool has_texture = !fitted.empty() && m.hasUvs();
  if (has_texture && !writePngFile(prefix + ".png", fitted)) return false;

  // MTL. A UsdPreviewSurface-equivalent Phong block, which is what every OBJ
  // consumer understands.
  std::ostringstream mtl;
  mtl << "# generated by " << options.generator << "\n";
  mtl << "newmtl " << options.name << "\n";
  mtl << "Ka 0.000 0.000 0.000\n";
  mtl << "Kd 1.000 1.000 1.000\n";
  mtl << "Ks 0.000 0.000 0.000\n";
  mtl << "Ns 10.0\n";
  mtl << "d 1.0\n";
  mtl << "illum 2\n";
  if (has_texture) mtl << "map_Kd " << base_name << ".png\n";
  if (!writeFileText(prefix + ".mtl", mtl.str())) return false;

  std::ostringstream obj;
  obj << "# generated by " << options.generator << "\n";
  obj << "# units: " << unitName(options.unit) << "\n";
  obj << "mtllib " << base_name << ".mtl\n";
  obj << "o " << options.name << "\n";

  const bool colors = m.hasColors() && options.include_vertex_colors;
  for (size_t i = 0; i < m.vertexCount(); ++i) {
    obj << "v " << formatFloat(m.vertices[i].x()) << ' ' << formatFloat(m.vertices[i].y())
        << ' ' << formatFloat(m.vertices[i].z());
    if (colors) {
      // The widely supported (if unofficial) extension: linear RGB after xyz.
      obj << ' ' << formatFloat(m.colors[i][0] / 255.0, 4) << ' '
          << formatFloat(m.colors[i][1] / 255.0, 4) << ' '
          << formatFloat(m.colors[i][2] / 255.0, 4);
    }
    obj << '\n';
  }
  if (m.hasUvs()) {
    for (const Vec2f& uv : m.uvs) {
      // OBJ's V axis points up, glTF's points down.
      obj << "vt " << formatFloat(uv.x()) << ' ' << formatFloat(1.0 - uv.y()) << '\n';
    }
  }
  if (m.hasNormals()) {
    for (const Vec3f& n : m.normals) {
      obj << "vn " << formatFloat(n.x()) << ' ' << formatFloat(n.y()) << ' '
          << formatFloat(n.z()) << '\n';
    }
  }
  obj << "usemtl " << options.name << "\n";
  obj << "s off\n";
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    obj << 'f';
    for (int k = 0; k < 3; ++k) {
      // OBJ indices are 1-based, and the v/vt/vn triplets are parallel here.
      const uint32_t index = m.indices[t + k] + 1;
      obj << ' ' << index;
      if (m.hasUvs() || m.hasNormals()) {
        obj << '/';
        if (m.hasUvs()) obj << index;
        if (m.hasNormals()) obj << '/' << index;
      }
    }
    obj << '\n';
  }
  return writeFileText(prefix + ".obj", obj.str());
}

}  // namespace parallax
