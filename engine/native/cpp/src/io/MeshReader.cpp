#include "parallax/io/MeshReader.h"

#include "parallax/io/ImageIo.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace parallax {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

/// One property of a PLY element.
struct PlyProperty {
  std::string name;
  std::string type;
  bool is_list = false;
  std::string count_type;
};

struct PlyElement {
  std::string name;
  size_t count = 0;
  std::vector<PlyProperty> properties;
};

size_t plyTypeSize(const std::string& type) {
  if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
  if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
  if (type == "int" || type == "uint" || type == "int32" || type == "uint32" ||
      type == "float" || type == "float32") {
    return 4;
  }
  if (type == "double" || type == "float64") return 8;
  return 0;
}

/// Read one scalar of the named PLY type from a binary little-endian buffer.
bool readPlyScalar(const std::vector<uint8_t>& bytes, size_t* pos, const std::string& type,
                   double* out) {
  const size_t size = plyTypeSize(type);
  if (size == 0 || *pos + size > bytes.size()) return false;
  const uint8_t* p = bytes.data() + *pos;
  if (type == "char" || type == "int8") {
    *out = static_cast<double>(static_cast<int8_t>(p[0]));
  } else if (type == "uchar" || type == "uint8") {
    *out = static_cast<double>(p[0]);
  } else if (type == "short" || type == "int16") {
    int16_t v;
    std::memcpy(&v, p, 2);
    *out = v;
  } else if (type == "ushort" || type == "uint16") {
    uint16_t v;
    std::memcpy(&v, p, 2);
    *out = v;
  } else if (type == "int" || type == "int32") {
    int32_t v;
    std::memcpy(&v, p, 4);
    *out = v;
  } else if (type == "uint" || type == "uint32") {
    uint32_t v;
    std::memcpy(&v, p, 4);
    *out = v;
  } else if (type == "float" || type == "float32") {
    float v;
    std::memcpy(&v, p, 4);
    *out = v;
  } else if (type == "double" || type == "float64") {
    double v;
    std::memcpy(&v, p, 8);
    *out = v;
  } else {
    return false;
  }
  *pos += size;
  return true;
}

/// Parse a PLY header, returning the body offset.
bool parsePlyHeader(const std::vector<uint8_t>& bytes, std::vector<PlyElement>* elements,
                    bool* binary, size_t* body_offset, std::string* error) {
  const std::string text(bytes.begin(),
                         bytes.begin() + std::min<size_t>(bytes.size(), 65536));
  if (text.compare(0, 3, "ply") != 0) return fail(error, "not a PLY file");

  size_t pos = 0;
  bool seen_format = false;
  while (true) {
    const size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) return fail(error, "unterminated PLY header");
    std::string line = text.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = eol + 1;

    std::istringstream ls(line);
    std::string token;
    ls >> token;
    if (token == "ply" || token == "comment" || token == "obj_info" || token.empty()) continue;
    if (token == "format") {
      std::string format;
      ls >> format;
      if (format == "ascii") {
        *binary = false;
      } else if (format == "binary_little_endian") {
        *binary = true;
      } else {
        return fail(error, "unsupported PLY format: " + format);
      }
      seen_format = true;
      continue;
    }
    if (token == "element") {
      PlyElement element;
      ls >> element.name >> element.count;
      elements->push_back(std::move(element));
      continue;
    }
    if (token == "property") {
      if (elements->empty()) return fail(error, "property before element");
      PlyProperty property;
      std::string type;
      ls >> type;
      if (type == "list") {
        property.is_list = true;
        ls >> property.count_type >> property.type >> property.name;
      } else {
        property.type = type;
        ls >> property.name;
      }
      if (plyTypeSize(property.type) == 0) {
        return fail(error, "unknown PLY property type: " + property.type);
      }
      elements->back().properties.push_back(std::move(property));
      continue;
    }
    if (token == "end_header") {
      *body_offset = pos;
      if (!seen_format) return fail(error, "PLY header has no format line");
      return true;
    }
    // Unknown header keyword: skip rather than fail, for forward compatibility.
  }
}

struct PlyData {
  std::vector<Vec3f> positions;
  std::vector<Vec3f> normals;
  std::vector<std::array<uint8_t, 3>> colors;
  std::vector<Vec2f> uvs;
  std::vector<uint32_t> indices;
};

bool readPlyBody(const std::vector<uint8_t>& bytes, const std::vector<PlyElement>& elements,
                 bool binary, size_t offset, PlyData* data, std::string* error) {
  // ASCII bodies are tokenised once; binary bodies are walked in place.
  std::istringstream ascii;
  if (!binary) {
    ascii.str(std::string(bytes.begin() + offset, bytes.end()));
  }
  size_t pos = offset;

  for (const PlyElement& element : elements) {
    // Guard against a header that declares more elements than could fit.
    if (binary) {
      size_t min_bytes = 0;
      for (const PlyProperty& p : element.properties) {
        min_bytes += p.is_list ? plyTypeSize(p.count_type) : plyTypeSize(p.type);
      }
      if (min_bytes > 0 && element.count > (bytes.size() - pos) / min_bytes + 1) {
        return fail(error, "PLY element count exceeds file size");
      }
    }

    for (size_t i = 0; i < element.count; ++i) {
      double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, u = 0, v = 0;
      double r = 255, g = 255, b = 255;
      bool has_normal = false, has_color = false, has_uv = false;

      for (const PlyProperty& property : element.properties) {
        if (property.is_list) {
          double count = 0;
          if (binary) {
            if (!readPlyScalar(bytes, &pos, property.count_type, &count)) {
              return fail(error, "truncated PLY list");
            }
          } else if (!(ascii >> count)) {
            return fail(error, "truncated PLY list");
          }
          const int n = static_cast<int>(count);
          if (n < 0 || n > 255) return fail(error, "implausible PLY list length");
          std::vector<uint32_t> face;
          face.reserve(n);
          for (int k = 0; k < n; ++k) {
            double index = 0;
            if (binary) {
              if (!readPlyScalar(bytes, &pos, property.type, &index)) {
                return fail(error, "truncated PLY list data");
              }
            } else if (!(ascii >> index)) {
              return fail(error, "truncated PLY list data");
            }
            if (index < 0) return fail(error, "negative PLY index");
            face.push_back(static_cast<uint32_t>(index));
          }
          // Fan-triangulate polygons with more than three corners.
          for (size_t k = 2; k < face.size(); ++k) {
            data->indices.push_back(face[0]);
            data->indices.push_back(face[k - 1]);
            data->indices.push_back(face[k]);
          }
          continue;
        }

        double value = 0;
        if (binary) {
          if (!readPlyScalar(bytes, &pos, property.type, &value)) {
            return fail(error, "truncated PLY element");
          }
        } else if (!(ascii >> value)) {
          return fail(error, "truncated PLY element");
        }

        const std::string& name = property.name;
        if (name == "x") x = value;
        else if (name == "y") y = value;
        else if (name == "z") z = value;
        else if (name == "nx") { nx = value; has_normal = true; }
        else if (name == "ny") { ny = value; has_normal = true; }
        else if (name == "nz") { nz = value; has_normal = true; }
        else if (name == "red" || name == "r") { r = value; has_color = true; }
        else if (name == "green" || name == "g") { g = value; has_color = true; }
        else if (name == "blue" || name == "b") { b = value; has_color = true; }
        else if (name == "s" || name == "u" || name == "texture_u") { u = value; has_uv = true; }
        else if (name == "t" || name == "v" || name == "texture_v") { v = value; has_uv = true; }
      }

      if (element.name == "vertex") {
        data->positions.push_back(Vec3f(static_cast<float>(x), static_cast<float>(y),
                                        static_cast<float>(z)));
        if (has_normal) {
          data->normals.push_back(Vec3f(static_cast<float>(nx), static_cast<float>(ny),
                                        static_cast<float>(nz)));
        }
        if (has_color) {
          // Float colour channels are stored 0..1; integer ones 0..255.
          const auto quantize = [](double c) {
            const double scaled = c <= 1.0001 && c >= 0.0 ? c * 255.0 : c;
            return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(scaled))));
          };
          data->colors.push_back({quantize(r), quantize(g), quantize(b)});
        }
        if (has_uv) data->uvs.push_back(Vec2f(static_cast<float>(u), static_cast<float>(v)));
      }
    }
  }
  return true;
}

}  // namespace

bool readPlyBytes(const std::vector<uint8_t>& bytes, Mesh* out, std::string* error) {
  std::vector<PlyElement> elements;
  bool binary = false;
  size_t offset = 0;
  if (!parsePlyHeader(bytes, &elements, &binary, &offset, error)) return false;

  PlyData data;
  if (!readPlyBody(bytes, elements, binary, offset, &data, error)) return false;
  if (data.positions.empty()) return fail(error, "PLY has no vertices");

  out->clear();
  out->vertices = std::move(data.positions);
  if (data.normals.size() == out->vertices.size()) out->normals = std::move(data.normals);
  if (data.colors.size() == out->vertices.size()) out->colors = std::move(data.colors);
  if (data.uvs.size() == out->vertices.size()) out->uvs = std::move(data.uvs);
  out->indices = std::move(data.indices);

  for (uint32_t i : out->indices) {
    if (i >= out->vertices.size()) return fail(error, "PLY face index out of range");
  }
  return true;
}

bool readPly(const std::string& path, Mesh* out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return fail(error, "cannot read " + path);
  return readPlyBytes(bytes, out, error);
}

bool readPlyPointCloud(const std::string& path, std::vector<Vec3f>* points,
                       std::vector<Vec3f>* normals,
                       std::vector<std::array<uint8_t, 3>>* colors, std::string* error) {
  Mesh mesh;
  if (!readPly(path, &mesh, error)) return false;
  if (points) *points = mesh.vertices;
  if (normals) *normals = mesh.normals;
  if (colors) *colors = mesh.colors;
  return true;
}

bool readStl(const std::string& path, Mesh* out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return fail(error, "cannot read " + path);
  if (bytes.size() < 15) return fail(error, "file too short for STL");

  // Distinguish binary from ASCII by checking whether the declared triangle
  // count matches the file size, rather than by sniffing for "solid": binary
  // STL files written by other tools frequently start with that word too.
  bool is_binary = false;
  if (bytes.size() >= 84) {
    uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4);
    if (static_cast<size_t>(84) + static_cast<size_t>(count) * 50 == bytes.size()) {
      is_binary = true;
    }
  }

  out->clear();
  if (is_binary) {
    uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4);
    out->vertices.reserve(static_cast<size_t>(count) * 3);
    out->indices.reserve(static_cast<size_t>(count) * 3);
    size_t pos = 84;
    for (uint32_t t = 0; t < count; ++t) {
      pos += 12;  // skip the stored facet normal; we recompute from geometry
      for (int k = 0; k < 3; ++k) {
        float xyz[3];
        std::memcpy(xyz, bytes.data() + pos, 12);
        pos += 12;
        out->indices.push_back(static_cast<uint32_t>(out->vertices.size()));
        out->vertices.push_back(Vec3f(xyz[0], xyz[1], xyz[2]));
      }
      pos += 2;  // attribute byte count
    }
  } else {
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream ls(text);
    std::string token;
    std::vector<Vec3f> loop;
    while (ls >> token) {
      if (token != "vertex") continue;
      double x = 0, y = 0, z = 0;
      if (!(ls >> x >> y >> z)) return fail(error, "malformed ASCII STL vertex");
      loop.push_back(
          Vec3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
      if (loop.size() == 3) {
        for (const Vec3f& v : loop) {
          out->indices.push_back(static_cast<uint32_t>(out->vertices.size()));
          out->vertices.push_back(v);
        }
        loop.clear();
      }
    }
  }
  if (out->vertices.empty()) return fail(error, "STL contains no triangles");

  // STL is a triangle soup with no shared vertices at all; weld so the result
  // has usable topology.
  weldVertices(out, 1e-6);
  recomputeNormals(out);
  return true;
}

bool readObj(const std::string& path, Mesh* out, std::string* error) {
  std::string text;
  if (!readFileText(path, &text)) return fail(error, "cannot read " + path);

  std::vector<Vec3f> positions;
  std::vector<Vec3f> normals;
  std::vector<Vec2f> uvs;
  std::vector<std::array<uint8_t, 3>> colors;
  bool any_vertex_color = false;

  struct Corner {
    int v = 0, t = 0, n = 0;
  };
  std::vector<std::array<Corner, 3>> faces;

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string keyword;
    ls >> keyword;

    if (keyword == "v") {
      double x = 0, y = 0, z = 0;
      if (!(ls >> x >> y >> z)) return fail(error, "malformed OBJ 'v'");
      positions.push_back(
          Vec3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
      // The vertex-colour extension appends r g b in 0..1.
      double r = 0, g = 0, b = 0;
      if (ls >> r >> g >> b) {
        any_vertex_color = true;
        const auto q = [](double c) {
          return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(c * 255.0))));
        };
        colors.push_back({q(r), q(g), q(b)});
      } else {
        colors.push_back({255, 255, 255});
      }
      continue;
    }
    if (keyword == "vt") {
      double u = 0, v = 0;
      if (!(ls >> u >> v)) return fail(error, "malformed OBJ 'vt'");
      // OBJ's V axis points up; the engine uses glTF's downward convention.
      uvs.push_back(Vec2f(static_cast<float>(u), static_cast<float>(1.0 - v)));
      continue;
    }
    if (keyword == "vn") {
      double x = 0, y = 0, z = 0;
      if (!(ls >> x >> y >> z)) return fail(error, "malformed OBJ 'vn'");
      normals.push_back(
          Vec3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
      continue;
    }
    if (keyword != "f") continue;

    // Parse the corners of this face: v, v/vt, v//vn or v/vt/vn.
    std::vector<Corner> corners;
    std::string chunk;
    while (ls >> chunk) {
      Corner corner;
      // Replace slashes with spaces, remembering where they were so an empty
      // vt field is not mistaken for the vn field.
      size_t first = chunk.find('/');
      if (first == std::string::npos) {
        corner.v = std::atoi(chunk.c_str());
      } else {
        corner.v = std::atoi(chunk.substr(0, first).c_str());
        const size_t second = chunk.find('/', first + 1);
        if (second == std::string::npos) {
          corner.t = std::atoi(chunk.substr(first + 1).c_str());
        } else {
          if (second > first + 1) {
            corner.t = std::atoi(chunk.substr(first + 1, second - first - 1).c_str());
          }
          corner.n = std::atoi(chunk.substr(second + 1).c_str());
        }
      }
      corners.push_back(corner);
    }
    if (corners.size() < 3) continue;
    for (size_t k = 2; k < corners.size(); ++k) {
      faces.push_back({corners[0], corners[k - 1], corners[k]});
    }
  }
  if (positions.empty()) return fail(error, "OBJ has no vertices");

  // OBJ indexes positions, uvs and normals independently, so a vertex must be
  // created per unique (v, vt, vn) combination.
  out->clear();
  std::map<std::array<int, 3>, uint32_t> lookup;
  const auto resolve = [&](int index, size_t size) -> int {
    if (index > 0) return index - 1;
    if (index < 0) return static_cast<int>(size) + index;  // negative is relative to the end
    return -1;
  };

  const bool want_uvs = !uvs.empty();
  const bool want_normals = !normals.empty();
  for (const auto& face : faces) {
    for (const Corner& corner : face) {
      const int vi = resolve(corner.v, positions.size());
      const int ti = resolve(corner.t, uvs.size());
      const int ni = resolve(corner.n, normals.size());
      if (vi < 0 || vi >= static_cast<int>(positions.size())) {
        return fail(error, "OBJ face references an unknown vertex");
      }
      const std::array<int, 3> key{vi, ti, ni};
      const auto it = lookup.find(key);
      if (it != lookup.end()) {
        out->indices.push_back(it->second);
        continue;
      }
      const uint32_t id = static_cast<uint32_t>(out->vertices.size());
      out->vertices.push_back(positions[vi]);
      if (any_vertex_color) out->colors.push_back(colors[vi]);
      if (want_uvs) {
        out->uvs.push_back(ti >= 0 && ti < static_cast<int>(uvs.size()) ? uvs[ti]
                                                                       : Vec2f::Zero());
      }
      if (want_normals) {
        out->normals.push_back(ni >= 0 && ni < static_cast<int>(normals.size())
                                   ? normals[ni]
                                   : Vec3f(0.f, 1.f, 0.f));
      }
      lookup.emplace(key, id);
      out->indices.push_back(id);
    }
  }
  if (!want_normals) recomputeNormals(out);
  return true;
}

bool readGlb(const std::string& path, GlbFile* out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return fail(error, "cannot read " + path);
  if (bytes.size() < 20) return fail(error, "file too short for GLB");

  uint32_t magic = 0, version = 0, total = 0;
  std::memcpy(&magic, bytes.data(), 4);
  std::memcpy(&version, bytes.data() + 4, 4);
  std::memcpy(&total, bytes.data() + 8, 4);
  if (magic != 0x46546C67) return fail(error, "bad GLB magic");
  if (version != 2) return fail(error, "unsupported GLB version");
  if (total != bytes.size()) {
    return fail(error, "GLB declared length does not match the file size");
  }

  size_t pos = 12;
  bool seen_json = false;
  while (pos + 8 <= bytes.size()) {
    uint32_t length = 0, type = 0;
    std::memcpy(&length, bytes.data() + pos, 4);
    std::memcpy(&type, bytes.data() + pos + 4, 4);
    if (length % 4 != 0) return fail(error, "GLB chunk length is not 4-byte aligned");
    if (pos + 8 + length > bytes.size()) return fail(error, "GLB chunk overruns the file");
    const uint8_t* data = bytes.data() + pos + 8;

    if (type == 0x4E4F534A) {  // "JSON"
      if (seen_json) return fail(error, "GLB has more than one JSON chunk");
      const std::string text(reinterpret_cast<const char*>(data), length);
      std::string parse_error;
      if (!Json::parse(text, &out->json, &parse_error)) {
        return fail(error, "GLB JSON chunk is malformed: " + parse_error);
      }
      seen_json = true;
    } else if (type == 0x004E4942) {  // "BIN\0"
      out->binary.assign(data, data + length);
    }
    pos += 8 + length;
  }
  if (!seen_json) return fail(error, "GLB has no JSON chunk");
  if (pos != bytes.size()) return fail(error, "trailing bytes after the last GLB chunk");
  return true;
}

bool validateGltf(const Json& document, size_t binary_length, std::string* error) {
  const auto bad = [error](const std::string& message) {
    if (error) *error = message;
    return false;
  };

  if (!document.isObject()) return bad("glTF root is not an object");
  const Json& asset = document.get("asset");
  if (!asset.isObject()) return bad("missing asset");
  if (asset.get("version").asString() != "2.0") return bad("asset.version must be \"2.0\"");

  // ---- buffers ------------------------------------------------------------
  const Json& buffers = document.get("buffers");
  if (!buffers.isArray() || buffers.size() == 0) return bad("missing buffers");
  std::vector<int64_t> buffer_lengths;
  for (size_t i = 0; i < buffers.size(); ++i) {
    const Json& buffer = buffers.at(i);
    const int64_t length = buffer.get("byteLength").asInt(-1);
    if (length < 0) return bad("buffer has no byteLength");
    // For GLB, buffer 0 has no uri and must fit the BIN chunk. The chunk may be
    // padded up to 3 bytes beyond byteLength, so allow that slack.
    if (i == 0 && !buffer.has("uri")) {
      if (static_cast<size_t>(length) > binary_length) {
        return bad("buffer 0 byteLength exceeds the BIN chunk");
      }
      if (binary_length - static_cast<size_t>(length) > 3) {
        return bad("BIN chunk is larger than buffer 0 plus its padding");
      }
    }
    buffer_lengths.push_back(length);
  }

  // ---- buffer views -------------------------------------------------------
  const Json& views = document.get("bufferViews");
  if (!views.isArray()) return bad("missing bufferViews");
  struct ViewInfo {
    int64_t offset = 0, length = 0;
  };
  std::vector<ViewInfo> view_infos;
  for (size_t i = 0; i < views.size(); ++i) {
    const Json& view = views.at(i);
    const int64_t buffer = view.get("buffer").asInt(-1);
    const int64_t offset = view.get("byteOffset").asInt(0);
    const int64_t length = view.get("byteLength").asInt(-1);
    if (buffer < 0 || buffer >= static_cast<int64_t>(buffer_lengths.size())) {
      return bad("bufferView references an unknown buffer");
    }
    if (length <= 0) return bad("bufferView has a non-positive byteLength");
    if (offset < 0 || offset + length > buffer_lengths[buffer]) {
      return bad("bufferView runs past the end of its buffer");
    }
    view_infos.push_back(ViewInfo{offset, length});
  }

  // ---- accessors ----------------------------------------------------------
  const Json& accessors = document.get("accessors");
  if (!accessors.isArray()) return bad("missing accessors");
  const auto componentSize = [](int64_t type) -> int {
    switch (type) {
      case 5120: case 5121: return 1;  // BYTE, UNSIGNED_BYTE
      case 5122: case 5123: return 2;  // SHORT, UNSIGNED_SHORT
      case 5125: case 5126: return 4;  // UNSIGNED_INT, FLOAT
      default: return 0;
    }
  };
  const auto componentCount = [](const std::string& type) -> int {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
  };

  struct AccessorInfo {
    int64_t count = 0;
    int64_t component_type = 0;
    std::string type;
    bool has_min_max = false;
  };
  std::vector<AccessorInfo> accessor_infos;
  for (size_t i = 0; i < accessors.size(); ++i) {
    const Json& accessor = accessors.at(i);
    const int64_t view = accessor.get("bufferView").asInt(-1);
    const int64_t count = accessor.get("count").asInt(-1);
    const int64_t component_type = accessor.get("componentType").asInt(0);
    const std::string type = accessor.get("type").asString();
    const int64_t offset = accessor.get("byteOffset").asInt(0);

    if (count <= 0) return bad("accessor has a non-positive count");
    const int element_size = componentSize(component_type);
    const int components = componentCount(type);
    if (element_size == 0) return bad("accessor has an unknown componentType");
    if (components == 0) return bad("accessor has an unknown type: " + type);
    if (view < 0 || view >= static_cast<int64_t>(view_infos.size())) {
      return bad("accessor references an unknown bufferView");
    }
    // The spec requires the accessor's offset into the buffer to be a multiple
    // of the component size.
    if ((view_infos[view].offset + offset) % element_size != 0) {
      return bad("accessor byteOffset is not aligned to its component size");
    }
    const int64_t needed = offset + count * element_size * components;
    if (needed > view_infos[view].length) {
      return bad("accessor runs past the end of its bufferView");
    }
    accessor_infos.push_back(AccessorInfo{count, component_type, type,
                                          accessor.has("min") && accessor.has("max")});
  }

  // ---- meshes -------------------------------------------------------------
  const Json& meshes = document.get("meshes");
  if (!meshes.isArray() || meshes.size() == 0) return bad("missing meshes");
  for (size_t m = 0; m < meshes.size(); ++m) {
    const Json& primitives = meshes.at(m).get("primitives");
    if (!primitives.isArray() || primitives.size() == 0) {
      return bad("mesh has no primitives");
    }
    for (size_t p = 0; p < primitives.size(); ++p) {
      const Json& primitive = primitives.at(p);
      const Json& attributes = primitive.get("attributes");
      if (!attributes.isObject()) return bad("primitive has no attributes");

      const Json& position = attributes.get("POSITION");
      if (!position.isNumber()) return bad("primitive has no POSITION attribute");
      const int64_t position_index = position.asInt(-1);
      if (position_index < 0 || position_index >= static_cast<int64_t>(accessor_infos.size())) {
        return bad("POSITION references an unknown accessor");
      }
      const AccessorInfo& position_info = accessor_infos[position_index];
      if (position_info.type != "VEC3" || position_info.component_type != 5126) {
        return bad("POSITION must be a float VEC3");
      }
      // Required by the spec, and relied on by viewers to frame the model.
      if (!position_info.has_min_max) return bad("POSITION accessor needs min and max");

      // Every attribute must have the same element count as POSITION.
      for (const auto& entry : attributes.members()) {
        const int64_t index = entry.second.asInt(-1);
        if (index < 0 || index >= static_cast<int64_t>(accessor_infos.size())) {
          return bad("attribute " + entry.first + " references an unknown accessor");
        }
        if (accessor_infos[index].count != position_info.count) {
          return bad("attribute " + entry.first + " count differs from POSITION");
        }
      }

      if (primitive.has("indices")) {
        const int64_t index_accessor = primitive.get("indices").asInt(-1);
        if (index_accessor < 0 ||
            index_accessor >= static_cast<int64_t>(accessor_infos.size())) {
          return bad("indices reference an unknown accessor");
        }
        const AccessorInfo& info = accessor_infos[index_accessor];
        if (info.type != "SCALAR") return bad("index accessor must be SCALAR");
        if (info.component_type != 5121 && info.component_type != 5123 &&
            info.component_type != 5125) {
          return bad("index accessor has an invalid componentType");
        }
        const int64_t mode = primitive.get("mode").asInt(4);
        if (mode == 4 && info.count % 3 != 0) {
          return bad("triangle primitive index count is not a multiple of 3");
        }
      }
    }
  }

  // ---- scene graph --------------------------------------------------------
  const Json& nodes = document.get("nodes");
  if (!nodes.isArray() || nodes.size() == 0) return bad("missing nodes");
  const Json& scenes = document.get("scenes");
  if (!scenes.isArray() || scenes.size() == 0) return bad("missing scenes");
  const int64_t scene = document.get("scene").asInt(-1);
  if (scene < 0 || scene >= static_cast<int64_t>(scenes.size())) {
    return bad("scene index is out of range");
  }
  for (size_t s = 0; s < scenes.size(); ++s) {
    const Json& scene_nodes = scenes.at(s).get("nodes");
    if (!scene_nodes.isArray()) return bad("scene has no nodes array");
    for (size_t i = 0; i < scene_nodes.size(); ++i) {
      const int64_t index = scene_nodes.at(i).asInt(-1);
      if (index < 0 || index >= static_cast<int64_t>(nodes.size())) {
        return bad("scene references an unknown node");
      }
    }
  }

  if (error) error->clear();
  return true;
}

}  // namespace parallax
