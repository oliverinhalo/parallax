// glTF 2.0 and USDZ writers.
#include "parallax/exporters/MeshExport.h"

#include "parallax/io/ImageIo.h"
#include "parallax/io/Json.h"
#include "parallax/io/Zip.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace parallax {
namespace {

// glTF component types.
constexpr int kComponentUnsignedInt = 5125;
constexpr int kComponentFloat = 5126;
constexpr int kComponentUnsignedByte = 5121;
// glTF target hints.
constexpr int kTargetArrayBuffer = 34962;
constexpr int kTargetElementArrayBuffer = 34963;
constexpr int kModeTriangles = 4;

template <typename T>
void appendRaw(std::vector<uint8_t>* out, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

void padTo(std::vector<uint8_t>* out, size_t alignment, uint8_t filler = 0) {
  while (out->size() % alignment != 0) out->push_back(filler);
}

ColorBuffer fitTextureForExport(const ColorBuffer& texture, int max_size) {
  if (texture.empty() || max_size <= 0) return texture;
  if (std::max(texture.width, texture.height) <= max_size) return texture;
  const double scale = static_cast<double>(max_size) / std::max(texture.width, texture.height);
  const int w = std::max(1, static_cast<int>(std::lround(texture.width * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(texture.height * scale)));
  ColorBuffer out(w, h);
  for (int y = 0; y < h; ++y) {
    const int sy = std::min(texture.height - 1, y * texture.height / h);
    for (int x = 0; x < w; ++x) {
      const int sx = std::min(texture.width - 1, x * texture.width / w);
      uint8_t rgb[3];
      texture.get(sx, sy, rgb);
      out.set(x, y, rgb[0], rgb[1], rgb[2]);
    }
  }
  return out;
}

/// Accumulates the glTF binary buffer and the accessor/bufferView metadata.
class GltfBuilder {
 public:
  GltfBuilder(const Mesh& mesh, const ColorBuffer& texture, const ExportOptions& options)
      : mesh_(mesh), texture_(texture), options_(options) {}

  /// Build the JSON document. `buffer_uri` is empty for GLB (the buffer comes
  /// from the BIN chunk) or a filename for the .gltf + .bin layout.
  Json build(const std::string& buffer_uri);

  const std::vector<uint8_t>& buffer() const { return buffer_; }
  const std::vector<uint8_t>& pngBytes() const { return png_; }
  bool hasTexture() const { return !png_.empty(); }

 private:
  /// Append data, 4-byte aligned, and return a bufferView index.
  int addBufferView(const void* data, size_t bytes, int target);
  int addAccessor(int buffer_view, int component_type, const std::string& type, size_t count,
                  const std::vector<double>& min, const std::vector<double>& max,
                  bool normalized = false);

  const Mesh& mesh_;
  const ColorBuffer& texture_;
  const ExportOptions& options_;
  std::vector<uint8_t> buffer_;
  std::vector<uint8_t> png_;
  Json buffer_views_ = Json::array();
  Json accessors_ = Json::array();
};

int GltfBuilder::addBufferView(const void* data, size_t bytes, int target) {
  // glTF requires each accessor's byte offset to be a multiple of the size of
  // its component type; aligning every view to 4 satisfies that for all the
  // types used here.
  padTo(&buffer_, 4);
  const size_t offset = buffer_.size();
  const auto* src = static_cast<const uint8_t*>(data);
  buffer_.insert(buffer_.end(), src, src + bytes);

  Json view = Json::object();
  view.set("buffer", Json::integer(0));
  view.set("byteOffset", Json::integer(static_cast<int64_t>(offset)));
  view.set("byteLength", Json::integer(static_cast<int64_t>(bytes)));
  if (target != 0) view.set("target", Json::integer(target));
  const int index = static_cast<int>(buffer_views_.size());
  buffer_views_.push(std::move(view));
  return index;
}

int GltfBuilder::addAccessor(int buffer_view, int component_type, const std::string& type,
                             size_t count, const std::vector<double>& min,
                             const std::vector<double>& max, bool normalized) {
  Json accessor = Json::object();
  accessor.set("bufferView", Json::integer(buffer_view));
  accessor.set("componentType", Json::integer(component_type));
  accessor.set("count", Json::integer(static_cast<int64_t>(count)));
  accessor.set("type", Json::string(type));
  if (normalized) accessor.set("normalized", Json::boolean(true));
  // POSITION accessors are *required* by the spec to carry min and max; many
  // viewers use them to frame the model and will reject the file without them.
  if (!min.empty()) {
    Json min_array = Json::array();
    for (double v : min) min_array.push(Json::number(v));
    accessor.set("min", std::move(min_array));
  }
  if (!max.empty()) {
    Json max_array = Json::array();
    for (double v : max) max_array.push(Json::number(v));
    accessor.set("max", std::move(max_array));
  }
  const int index = static_cast<int>(accessors_.size());
  accessors_.push(std::move(accessor));
  return index;
}

Json GltfBuilder::build(const std::string& buffer_uri) {
  // ---- indices ------------------------------------------------------------
  const int index_view =
      addBufferView(mesh_.indices.data(), mesh_.indices.size() * sizeof(uint32_t),
                    kTargetElementArrayBuffer);
  const int index_accessor = addAccessor(index_view, kComponentUnsignedInt, "SCALAR",
                                         mesh_.indices.size(), {}, {});

  // ---- positions ----------------------------------------------------------
  std::vector<float> positions;
  positions.reserve(mesh_.vertexCount() * 3);
  double lo[3] = {1e300, 1e300, 1e300};
  double hi[3] = {-1e300, -1e300, -1e300};
  for (const Vec3f& v : mesh_.vertices) {
    const float xyz[3] = {v.x(), v.y(), v.z()};
    for (int c = 0; c < 3; ++c) {
      positions.push_back(xyz[c]);
      lo[c] = std::min(lo[c], static_cast<double>(xyz[c]));
      hi[c] = std::max(hi[c], static_cast<double>(xyz[c]));
    }
  }
  const int position_view =
      addBufferView(positions.data(), positions.size() * sizeof(float), kTargetArrayBuffer);
  const int position_accessor =
      addAccessor(position_view, kComponentFloat, "VEC3", mesh_.vertexCount(),
                  {lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]});

  Json attributes = Json::object();
  attributes.set("POSITION", Json::integer(position_accessor));

  if (mesh_.hasNormals()) {
    std::vector<float> normals;
    normals.reserve(mesh_.vertexCount() * 3);
    for (const Vec3f& n : mesh_.normals) {
      normals.push_back(n.x());
      normals.push_back(n.y());
      normals.push_back(n.z());
    }
    const int view =
        addBufferView(normals.data(), normals.size() * sizeof(float), kTargetArrayBuffer);
    attributes.set("NORMAL", Json::integer(addAccessor(view, kComponentFloat, "VEC3",
                                                       mesh_.vertexCount(), {}, {})));
  }
  if (mesh_.hasUvs()) {
    std::vector<float> uvs;
    uvs.reserve(mesh_.vertexCount() * 2);
    for (const Vec2f& uv : mesh_.uvs) {
      uvs.push_back(uv.x());
      uvs.push_back(uv.y());
    }
    const int view = addBufferView(uvs.data(), uvs.size() * sizeof(float), kTargetArrayBuffer);
    attributes.set("TEXCOORD_0", Json::integer(addAccessor(view, kComponentFloat, "VEC2",
                                                           mesh_.vertexCount(), {}, {})));
  }
  if (mesh_.hasColors() && options_.include_vertex_colors) {
    // COLOR_0 as normalized unsigned bytes. VEC4 rather than VEC3 because a
    // number of viewers quietly ignore 3-component vertex colour.
    std::vector<uint8_t> colors;
    colors.reserve(mesh_.vertexCount() * 4);
    for (const auto& c : mesh_.colors) {
      colors.push_back(c[0]);
      colors.push_back(c[1]);
      colors.push_back(c[2]);
      colors.push_back(255);
    }
    const int view = addBufferView(colors.data(), colors.size(), kTargetArrayBuffer);
    attributes.set("COLOR_0",
                   Json::integer(addAccessor(view, kComponentUnsignedByte, "VEC4",
                                             mesh_.vertexCount(), {}, {}, true)));
  }

  // ---- texture ------------------------------------------------------------
  const ColorBuffer fitted = fitTextureForExport(texture_, options_.max_texture_size);
  int image_view = -1;
  if (!fitted.empty() && mesh_.hasUvs()) {
    if (encodePng(fitted, &png_)) {
      if (buffer_uri.empty()) {
        // GLB: the PNG lives inside the same binary buffer.
        image_view = addBufferView(png_.data(), png_.size(), 0);
      }
    }
  }

  // ---- document -----------------------------------------------------------
  Json root = Json::object();
  Json asset = Json::object();
  asset.set("version", Json::string("2.0"));
  asset.set("generator", Json::string(options_.generator));
  root.set("asset", std::move(asset));

  Json buffer = Json::object();
  buffer.set("byteLength", Json::integer(static_cast<int64_t>(buffer_.size())));
  if (!buffer_uri.empty()) buffer.set("uri", Json::string(buffer_uri));
  Json buffers = Json::array();
  buffers.push(std::move(buffer));
  root.set("buffers", std::move(buffers));
  root.set("bufferViews", buffer_views_);
  root.set("accessors", accessors_);

  Json material = Json::object();
  material.set("name", Json::string(options_.name + "Material"));
  material.set("doubleSided", Json::boolean(true));
  Json pbr = Json::object();
  Json base_color = Json::array();
  for (int i = 0; i < 3; ++i) base_color.push(Json::number(1.0));
  base_color.push(Json::number(1.0));
  pbr.set("baseColorFactor", std::move(base_color));
  // A scanned surface is a dielectric with no specular highlight to speak of.
  pbr.set("metallicFactor", Json::number(0.0));
  pbr.set("roughnessFactor", Json::number(0.9));

  if (!png_.empty()) {
    Json image = Json::object();
    if (image_view >= 0) {
      image.set("bufferView", Json::integer(image_view));
      image.set("mimeType", Json::string("image/png"));
    } else {
      image.set("uri", Json::string(options_.name + ".png"));
    }
    Json images = Json::array();
    images.push(std::move(image));
    root.set("images", std::move(images));

    Json sampler = Json::object();
    sampler.set("magFilter", Json::integer(9729));  // LINEAR
    sampler.set("minFilter", Json::integer(9987));  // LINEAR_MIPMAP_LINEAR
    sampler.set("wrapS", Json::integer(33071));     // CLAMP_TO_EDGE
    sampler.set("wrapT", Json::integer(33071));
    Json samplers = Json::array();
    samplers.push(std::move(sampler));
    root.set("samplers", std::move(samplers));

    Json tex = Json::object();
    tex.set("sampler", Json::integer(0));
    tex.set("source", Json::integer(0));
    Json textures = Json::array();
    textures.push(std::move(tex));
    root.set("textures", std::move(textures));

    Json base_color_texture = Json::object();
    base_color_texture.set("index", Json::integer(0));
    base_color_texture.set("texCoord", Json::integer(0));
    pbr.set("baseColorTexture", std::move(base_color_texture));
  }
  material.set("pbrMetallicRoughness", std::move(pbr));
  if (options_.blend) {
    // The splat preview's soft edge is alpha in its texture, so the material
    // has to blend; BLEND rather than MASK because a hard cut-off would turn
    // every gaussian back into a square.
    material.set("alphaMode", Json::string("BLEND"));
  }
  if (options_.unlit) {
    // KHR_materials_unlit, declared on the material and in extensionsUsed.
    // The colour in a scan's vertices already includes its lighting; relighting
    // it makes the surface look like plastic.
    Json extensions = Json::object();
    extensions.set("KHR_materials_unlit", Json::object());
    material.set("extensions", std::move(extensions));
    Json used = Json::array();
    used.push(Json::string("KHR_materials_unlit"));
    root.set("extensionsUsed", std::move(used));
  }
  Json materials = Json::array();
  materials.push(std::move(material));
  root.set("materials", std::move(materials));

  Json primitive = Json::object();
  primitive.set("attributes", std::move(attributes));
  primitive.set("indices", Json::integer(index_accessor));
  primitive.set("material", Json::integer(0));
  primitive.set("mode", Json::integer(kModeTriangles));
  Json primitives = Json::array();
  primitives.push(std::move(primitive));

  Json gltf_mesh = Json::object();
  gltf_mesh.set("name", Json::string(options_.name));
  gltf_mesh.set("primitives", std::move(primitives));
  Json meshes = Json::array();
  meshes.push(std::move(gltf_mesh));
  root.set("meshes", std::move(meshes));

  Json node = Json::object();
  node.set("name", Json::string(options_.name));
  node.set("mesh", Json::integer(0));
  Json nodes = Json::array();
  nodes.push(std::move(node));
  root.set("nodes", std::move(nodes));

  Json scene = Json::object();
  Json scene_nodes = Json::array();
  scene_nodes.push(Json::integer(0));
  scene.set("nodes", std::move(scene_nodes));
  Json scenes = Json::array();
  scenes.push(std::move(scene));
  root.set("scenes", std::move(scenes));
  root.set("scene", Json::integer(0));

  return root;
}

}  // namespace

bool writeGlb(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
              const ExportOptions& options) {
  if (mesh.empty()) return false;
  const Mesh m = applyExportTransform(mesh, options);

  GltfBuilder builder(m, texture, options);
  const Json root = builder.build(/*buffer_uri=*/"");
  std::string json = root.dump();
  // The JSON chunk must be padded to 4 bytes with spaces (0x20), and the BIN
  // chunk with zeros. Getting the filler wrong is a validator error, not a
  // cosmetic detail.
  while (json.size() % 4 != 0) json.push_back(' ');

  std::vector<uint8_t> binary = builder.buffer();
  while (binary.size() % 4 != 0) binary.push_back(0);

  std::vector<uint8_t> out;
  const uint32_t total = static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size());
  appendRaw(&out, static_cast<uint32_t>(0x46546C67));  // "glTF"
  appendRaw(&out, static_cast<uint32_t>(2));           // version
  appendRaw(&out, total);

  appendRaw(&out, static_cast<uint32_t>(json.size()));
  appendRaw(&out, static_cast<uint32_t>(0x4E4F534A));  // "JSON"
  out.insert(out.end(), json.begin(), json.end());

  appendRaw(&out, static_cast<uint32_t>(binary.size()));
  appendRaw(&out, static_cast<uint32_t>(0x004E4942));  // "BIN\0"
  out.insert(out.end(), binary.begin(), binary.end());

  return writeFileBytes(path, out);
}

bool writeGltf(const std::string& directory, const std::string& base_name, const Mesh& mesh,
               const ColorBuffer& texture, const ExportOptions& options) {
  if (mesh.empty() || base_name.empty()) return false;
  const Mesh m = applyExportTransform(mesh, options);
  const std::string prefix = directory.empty() ? base_name : directory + "/" + base_name;

  GltfBuilder builder(m, texture, options);
  const Json root = builder.build(base_name + ".bin");
  if (!writeFileBytes(prefix + ".bin", builder.buffer())) return false;
  if (builder.hasTexture()) {
    if (!writeFileBytes(prefix + ".png", builder.pngBytes())) return false;
  }
  return writeFileText(prefix + ".gltf", root.dump(2));
}

// ------------------------------------------------------------------ USDZ ---

std::string buildUsda(const Mesh& mesh, const ExportOptions& options,
                      const std::string& texture_file) {
  std::ostringstream usd;
  const double metres_per_unit = 1.0 / unitScaleFromMetres(options.unit);

  usd << "#usda 1.0\n";
  usd << "(\n";
  usd << "    defaultPrim = \"" << options.name << "\"\n";
  usd << "    metersPerUnit = " << metres_per_unit << "\n";
  usd << "    upAxis = \"" << (options.up_axis == UpAxis::ZUp ? "Z" : "Y") << "\"\n";
  usd << "    doc = \"Generated by " << options.generator << "\"\n";
  usd << ")\n\n";

  usd << "def Xform \"" << options.name << "\" (\n";
  usd << "    kind = \"component\"\n";
  usd << ")\n{\n";
  usd << "    def Mesh \"geometry\"\n    {\n";

  // Per-face vertex counts: all triangles.
  usd << "        int[] faceVertexCounts = [";
  for (size_t t = 0; t < mesh.triangleCount(); ++t) {
    if (t) usd << ", ";
    usd << "3";
  }
  usd << "]\n";

  usd << "        int[] faceVertexIndices = [";
  for (size_t i = 0; i < mesh.indices.size(); ++i) {
    if (i) usd << ", ";
    usd << mesh.indices[i];
  }
  usd << "]\n";

  usd << "        point3f[] points = [";
  for (size_t i = 0; i < mesh.vertexCount(); ++i) {
    if (i) usd << ", ";
    usd << "(" << mesh.vertices[i].x() << ", " << mesh.vertices[i].y() << ", "
        << mesh.vertices[i].z() << ")";
  }
  usd << "]\n";

  if (mesh.hasNormals()) {
    usd << "        normal3f[] primvars:normals = [";
    for (size_t i = 0; i < mesh.normals.size(); ++i) {
      if (i) usd << ", ";
      usd << "(" << mesh.normals[i].x() << ", " << mesh.normals[i].y() << ", "
          << mesh.normals[i].z() << ")";
    }
    usd << "] (\n            interpolation = \"vertex\"\n        )\n";
  }
  if (mesh.hasUvs()) {
    // USD's st origin is bottom-left, glTF's is top-left.
    usd << "        texCoord2f[] primvars:st = [";
    for (size_t i = 0; i < mesh.uvs.size(); ++i) {
      if (i) usd << ", ";
      usd << "(" << mesh.uvs[i].x() << ", " << (1.0f - mesh.uvs[i].y()) << ")";
    }
    usd << "] (\n            interpolation = \"vertex\"\n        )\n";
  }
  if (mesh.hasColors() && options.include_vertex_colors) {
    usd << "        color3f[] primvars:displayColor = [";
    for (size_t i = 0; i < mesh.colors.size(); ++i) {
      if (i) usd << ", ";
      usd << "(" << mesh.colors[i][0] / 255.0f << ", " << mesh.colors[i][1] / 255.0f << ", "
          << mesh.colors[i][2] / 255.0f << ")";
    }
    usd << "] (\n            interpolation = \"vertex\"\n        )\n";
  }

  // A scan is a closed-ish surface with no sharp creases to preserve, so
  // subdivision is switched off and the triangles are taken literally.
  usd << "        uniform token subdivisionScheme = \"none\"\n";
  usd << "        rel material:binding = </" << options.name << "/material>\n";
  usd << "    }\n\n";

  usd << "    def Material \"material\"\n    {\n";
  usd << "        token outputs:surface.connect = </" << options.name
      << "/material/surface.outputs:surface>\n\n";
  usd << "        def Shader \"surface\"\n        {\n";
  usd << "            uniform token info:id = \"UsdPreviewSurface\"\n";
  if (!texture_file.empty() && mesh.hasUvs()) {
    usd << "            color3f inputs:diffuseColor.connect = </" << options.name
        << "/material/diffuseTexture.outputs:rgb>\n";
  } else {
    usd << "            color3f inputs:diffuseColor = (0.8, 0.8, 0.8)\n";
  }
  usd << "            float inputs:metallic = 0\n";
  usd << "            float inputs:roughness = 0.9\n";
  usd << "            token outputs:surface\n";
  usd << "        }\n";

  if (!texture_file.empty() && mesh.hasUvs()) {
    usd << "\n        def Shader \"uvReader\"\n        {\n";
    usd << "            uniform token info:id = \"UsdPrimvarReader_float2\"\n";
    usd << "            token inputs:varname = \"st\"\n";
    usd << "            float2 outputs:result\n";
    usd << "        }\n";
    usd << "\n        def Shader \"diffuseTexture\"\n        {\n";
    usd << "            uniform token info:id = \"UsdUVTexture\"\n";
    usd << "            asset inputs:file = @" << texture_file << "@\n";
    usd << "            float2 inputs:st.connect = </" << options.name
        << "/material/uvReader.outputs:result>\n";
    usd << "            token inputs:wrapS = \"clamp\"\n";
    usd << "            token inputs:wrapT = \"clamp\"\n";
    usd << "            float3 outputs:rgb\n";
    usd << "        }\n";
  }
  usd << "    }\n";
  usd << "}\n";
  return usd.str();
}

bool writeUsdz(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
               const ExportOptions& options) {
  if (mesh.empty()) return false;
  // USD's own convention is Y-up metres; honour the caller but note that
  // metersPerUnit in the stage header carries the unit, so geometry is scaled.
  const Mesh m = applyExportTransform(mesh, options);

  const ColorBuffer fitted = fitTextureForExport(texture, options.max_texture_size);
  std::vector<uint8_t> png;
  const bool has_texture =
      !fitted.empty() && m.hasUvs() && encodePng(fitted, &png);

  const std::string texture_name = has_texture ? "textures/diffuse.png" : "";
  const std::string usda = buildUsda(m, options, texture_name);

  // 64-byte alignment and STORE-only are both mandated by the USDZ spec so the
  // archive can be memory-mapped and handed to the USD runtime in place.
  ZipWriter zip(64);
  if (!zip.addText(options.name + ".usda", usda, ZipCompression::Store)) return false;
  if (has_texture) {
    if (!zip.add(texture_name, png, ZipCompression::Store)) return false;
  }
  return writeFileBytes(path, zip.finish());
}

}  // namespace parallax
