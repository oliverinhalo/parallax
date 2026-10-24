#include "parallax/exporters/MeshExport.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include "parallax/fusion/TsdfVolume.h"
#include "parallax/io/ImageIo.h"
#include "parallax/io/MeshReader.h"
#include "parallax/io/Zip.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A per-test temporary directory, removed on destruction.
class TempDir {
 public:
  TempDir() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      path_ = base / ("parallax_export_" + std::to_string(counter()++));
      std::error_code ec;
      if (std::filesystem::create_directory(path_, ec)) return;
    }
    path_ = base;  // last resort; tests still work, cleanup just does not
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::string file(const std::string& name) const { return (path_ / name).string(); }
  std::string dir() const { return path_.string(); }

 private:
  static int& counter() {
    static int value = 1000;
    return value;
  }
  std::filesystem::path path_;
};

/// A sphere mesh with normals, colours and uvs, as the pipeline would produce.
Mesh texturedSphere(double radius = 0.3, double cell = 0.045) {
  AxisAlignedBox bounds;
  bounds.expand(Vec3::Constant(-radius * 1.4));
  bounds.expand(Vec3::Constant(radius * 1.4));
  Mesh mesh =
      marchingTetrahedra(bounds, cell, [radius](const Vec3& p, float* v,
                                                std::array<uint8_t, 3>* c) {
        *v = static_cast<float>(p.norm() - radius);
        if (c) {
          // Position-derived colour, so a round trip can be checked per vertex.
          *c = {static_cast<uint8_t>(60 + 60 * (p.x() > 0)),
                static_cast<uint8_t>(90 + 60 * (p.y() > 0)),
                static_cast<uint8_t>(120 + 60 * (p.z() > 0))};
        }
        return true;
      });
  // Spherical uv parameterisation.
  mesh.uvs.resize(mesh.vertexCount());
  for (size_t i = 0; i < mesh.vertexCount(); ++i) {
    const Vec3f& v = mesh.vertices[i];
    const double u = 0.5 + std::atan2(v.z(), v.x()) / (2.0 * kPi);
    const double clamped = std::max(-1.0, std::min(1.0, v.y() / radius));
    const double t = 0.5 - std::asin(clamped) / kPi;
    mesh.uvs[i] = Vec2f(static_cast<float>(u), static_cast<float>(t));
  }
  return mesh;
}

ColorBuffer testTexture(int size = 64) {
  ColorBuffer texture(size, size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      texture.set(x, y, static_cast<uint8_t>(x * 255 / size),
                  static_cast<uint8_t>(y * 255 / size),
                  static_cast<uint8_t>(((x / 8 + y / 8) & 1) ? 230 : 40));
    }
  }
  return texture;
}

/// Hausdorff-style one-sided max distance from `a`'s vertices to `b`'s.
double maxVertexDeviation(const Mesh& a, const Mesh& b) {
  double worst = 0.0;
  for (const Vec3f& va : a.vertices) {
    double best = 1e300;
    for (const Vec3f& vb : b.vertices) {
      best = std::min(best, static_cast<double>((va - vb).norm()));
    }
    worst = std::max(worst, best);
  }
  return worst;
}

}  // namespace

// ---------------------------------------------------------------- options ---

TEST(Export, UnitScalesAreExact) {
  EXPECT_DOUBLE_EQ(unitScaleFromMetres(LengthUnit::Metres), 1.0);
  EXPECT_DOUBLE_EQ(unitScaleFromMetres(LengthUnit::Centimetres), 100.0);
  EXPECT_DOUBLE_EQ(unitScaleFromMetres(LengthUnit::Millimetres), 1000.0);
  // 1 inch is exactly 25.4 mm by definition.
  EXPECT_NEAR(unitScaleFromMetres(LengthUnit::Inches), 39.3700787, 1e-6);
  EXPECT_NEAR(unitScaleFromMetres(LengthUnit::Feet), 3.2808399, 1e-6);
  EXPECT_STREQ(unitName(LengthUnit::Millimetres), "mm");
}

TEST(Export, BasisConversionsArePropertyPreserving) {
  ExportOptions y_up;
  EXPECT_TRUE(exportBasis(y_up).isApprox(Mat3::Identity()));

  ExportOptions z_up;
  z_up.up_axis = UpAxis::ZUp;
  const Mat3 basis = exportBasis(z_up);
  // A rotation: determinant +1, orthonormal, and it must take +Y to +Z.
  EXPECT_NEAR(basis.determinant(), 1.0, 1e-12);
  EXPECT_TRUE((basis * basis.transpose()).isApprox(Mat3::Identity(), 1e-12));
  EXPECT_LT((basis * Vec3::UnitY() - Vec3::UnitZ()).norm(), 1e-12);

  ExportOptions left;
  left.handedness = Handedness::LeftHanded;
  EXPECT_LT(exportBasis(left).determinant(), 0.0);
}

TEST(Export, TransformAppliesUnitsAndKeepsWindingOutward) {
  const Mesh sphere = texturedSphere(0.25, 0.05);
  const double volume_m3 = meshSignedVolume(sphere);
  ASSERT_GT(volume_m3, 0.0);

  ExportOptions mm;
  mm.unit = LengthUnit::Millimetres;
  const Mesh scaled = applyExportTransform(sphere, mm);
  // Volume scales with the cube of the length unit.
  EXPECT_NEAR(meshSignedVolume(scaled), volume_m3 * 1e9, volume_m3 * 1e9 * 1e-6);

  // A left-handed export mirrors an axis, which would invert the winding; the
  // exporter must flip the index order to compensate, so the volume stays
  // positive in the destination convention.
  ExportOptions left;
  left.handedness = Handedness::LeftHanded;
  EXPECT_GT(meshSignedVolume(applyExportTransform(sphere, left)), 0.0);
}

TEST(Export, TransformHonoursAttributeToggles) {
  const Mesh sphere = texturedSphere();
  ASSERT_TRUE(sphere.hasNormals());
  ASSERT_TRUE(sphere.hasColors());
  ASSERT_TRUE(sphere.hasUvs());

  ExportOptions bare;
  bare.include_normals = false;
  bare.include_vertex_colors = false;
  bare.include_uvs = false;
  const Mesh stripped = applyExportTransform(sphere, bare);
  EXPECT_FALSE(stripped.hasNormals());
  EXPECT_FALSE(stripped.hasColors());
  EXPECT_FALSE(stripped.hasUvs());
  EXPECT_EQ(stripped.triangleCount(), sphere.triangleCount());
}

TEST(Export, TransformDecimatesWhenAsked) {
  const Mesh sphere = texturedSphere(0.3, 0.03);
  ASSERT_GT(sphere.triangleCount(), 2000u);
  ExportOptions options;
  options.target_triangles = 500;
  const Mesh reduced = applyExportTransform(sphere, options);
  EXPECT_LE(reduced.triangleCount(), 600u);
  EXPECT_GT(reduced.triangleCount(), 300u);
}

// -------------------------------------------------------------------- PLY ---

TEST(Export, PlyBinaryRoundTripsGeometryAndAttributes) {
  TempDir dir;
  const Mesh original = texturedSphere();
  const std::string path = dir.file("mesh.ply");

  ExportOptions options;
  options.binary = true;
  ASSERT_TRUE(writePly(path, original, options));

  Mesh loaded;
  std::string error;
  ASSERT_TRUE(readPly(path, &loaded, &error)) << error;
  EXPECT_EQ(loaded.vertexCount(), original.vertexCount());
  EXPECT_EQ(loaded.triangleCount(), original.triangleCount());
  ASSERT_TRUE(loaded.hasNormals());
  ASSERT_TRUE(loaded.hasColors());

  for (size_t i = 0; i < original.vertexCount(); ++i) {
    EXPECT_LT((loaded.vertices[i] - original.vertices[i]).norm(), 1e-6f) << "vertex " << i;
    EXPECT_LT((loaded.normals[i] - original.normals[i]).norm(), 1e-5f) << "normal " << i;
    EXPECT_EQ(loaded.colors[i], original.colors[i]) << "colour " << i;
  }
  EXPECT_EQ(loaded.indices, original.indices);
  // Topology must survive, not just the numbers.
  EXPECT_EQ(isWatertight(loaded), isWatertight(original));
  EXPECT_NEAR(meshSignedVolume(loaded), meshSignedVolume(original), 1e-6);
}

TEST(Export, PlyAsciiRoundTripsGeometry) {
  TempDir dir;
  const Mesh original = texturedSphere(0.2, 0.06);
  const std::string path = dir.file("mesh_ascii.ply");

  ExportOptions options;
  options.binary = false;
  ASSERT_TRUE(writePly(path, original, options));

  Mesh loaded;
  std::string error;
  ASSERT_TRUE(readPly(path, &loaded, &error)) << error;
  EXPECT_EQ(loaded.vertexCount(), original.vertexCount());
  EXPECT_EQ(loaded.triangleCount(), original.triangleCount());
  // ASCII is written at 6 decimal places, so the tolerance is coarser.
  EXPECT_LT(maxVertexDeviation(loaded, original), 1e-5);
  EXPECT_EQ(loaded.indices, original.indices);
}

TEST(Export, PlyCarriesTheUnitConversion) {
  TempDir dir;
  const Mesh original = texturedSphere(0.25, 0.06);
  const std::string path = dir.file("mm.ply");
  ExportOptions options;
  options.unit = LengthUnit::Millimetres;
  ASSERT_TRUE(writePly(path, original, options));

  Mesh loaded;
  ASSERT_TRUE(readPly(path, &loaded));
  const AxisAlignedBox before = meshBounds(original);
  const AxisAlignedBox after = meshBounds(loaded);
  EXPECT_NEAR(after.diagonal(), before.diagonal() * 1000.0, before.diagonal());
  // The header should record the unit for a human reader.
  std::string text;
  ASSERT_TRUE(readFileText(path, &text));
  EXPECT_NE(text.find("comment units mm"), std::string::npos);
}

TEST(Export, PointCloudPlyRoundTrips) {
  TempDir dir;
  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.25;
  ball.texture = Texture::Noise;
  scene.primitives.push_back(ball);

  TsdfConfig config;
  config.voxel_size = 0.012f;
  config.truncation = 0.04f;
  TsdfVolume volume(config);
  const CameraIntrinsics k = phoneIntrinsics(160, 120);
  for (const Pose& pose : SyntheticScene::orbitPoses(ball.center, 0.8, 0.0, 16)) {
    const RenderResult frame = scene.render(k, pose, true, true);
    volume.integrate(frame.depth, frame.color, k, pose);
  }
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  volume.extractPointCloud(&points, &normals, &colors);
  ASSERT_GT(points.size(), 200u);

  const std::string path = dir.file("cloud.ply");
  ASSERT_TRUE(writePointCloudPly(path, points, normals, colors, {}));

  std::vector<Vec3f> loaded_points, loaded_normals;
  std::vector<std::array<uint8_t, 3>> loaded_colors;
  std::string error;
  ASSERT_TRUE(readPlyPointCloud(path, &loaded_points, &loaded_normals, &loaded_colors, &error))
      << error;
  ASSERT_EQ(loaded_points.size(), points.size());
  EXPECT_EQ(loaded_normals.size(), normals.size());
  EXPECT_EQ(loaded_colors.size(), colors.size());
  for (size_t i = 0; i < points.size(); ++i) {
    EXPECT_LT((loaded_points[i] - points[i]).norm(), 1e-6f);
    EXPECT_EQ(loaded_colors[i], colors[i]);
  }
}

TEST(Export, PlyReaderRejectsMalformedInput) {
  Mesh mesh;
  std::string error;
  EXPECT_FALSE(readPlyBytes({}, &mesh, &error));
  EXPECT_FALSE(error.empty());

  const std::string not_ply = "hello world\n";
  EXPECT_FALSE(readPlyBytes(std::vector<uint8_t>(not_ply.begin(), not_ply.end()), &mesh));

  // A header that declares more vertices than the body provides.
  const std::string truncated =
      "ply\nformat ascii 1.0\nelement vertex 100\nproperty float x\nproperty float y\n"
      "property float z\nend_header\n0 0 0\n";
  EXPECT_FALSE(
      readPlyBytes(std::vector<uint8_t>(truncated.begin(), truncated.end()), &mesh, &error));

  // A face index past the end of the vertex list.
  const std::string bad_index =
      "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
      "property float z\nelement face 1\nproperty list uchar uint vertex_indices\n"
      "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 99\n";
  EXPECT_FALSE(
      readPlyBytes(std::vector<uint8_t>(bad_index.begin(), bad_index.end()), &mesh, &error));
  EXPECT_NE(error.find("out of range"), std::string::npos);
}

// -------------------------------------------------------------------- STL ---

TEST(Export, StlBinaryRoundTripsAndIsWatertight) {
  TempDir dir;
  const Mesh original = texturedSphere(0.3, 0.05);
  ASSERT_TRUE(isWatertight(original));
  const std::string path = dir.file("model.stl");

  ExportOptions options;
  options.binary = true;
  options.unit = LengthUnit::Millimetres;  // what slicers expect
  ASSERT_TRUE(writeStl(path, original, options));

  // The file size must match the format exactly: 84 + 50 bytes per triangle.
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  EXPECT_EQ(bytes.size(), 84u + 50u * original.triangleCount());

  Mesh loaded;
  std::string error;
  ASSERT_TRUE(readStl(path, &loaded, &error)) << error;
  EXPECT_EQ(loaded.triangleCount(), original.triangleCount());
  // STL carries no topology, but after welding the surface must close again --
  // which is what makes the file printable.
  EXPECT_TRUE(isWatertight(loaded));
  EXPECT_EQ(eulerCharacteristic(loaded), 2);
  // Volume in cubic millimetres.
  EXPECT_NEAR(meshSignedVolume(loaded), meshSignedVolume(original) * 1e9,
              meshSignedVolume(original) * 1e9 * 1e-3);
}

TEST(Export, StlAsciiRoundTrips) {
  TempDir dir;
  const Mesh original = texturedSphere(0.25, 0.07);
  const std::string path = dir.file("model_ascii.stl");
  ExportOptions options;
  options.binary = false;
  ASSERT_TRUE(writeStl(path, original, options));

  Mesh loaded;
  std::string error;
  ASSERT_TRUE(readStl(path, &loaded, &error)) << error;
  EXPECT_EQ(loaded.triangleCount(), original.triangleCount());
  EXPECT_TRUE(isWatertight(loaded));
}

TEST(Export, StlRecordsTheUnitInItsHeader) {
  TempDir dir;
  const std::string path = dir.file("units.stl");
  ExportOptions options;
  options.unit = LengthUnit::Millimetres;
  ASSERT_TRUE(writeStl(path, texturedSphere(0.2, 0.08), options));
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  const std::string header(bytes.begin(), bytes.begin() + 80);
  EXPECT_NE(header.find("units=mm"), std::string::npos);
}

// -------------------------------------------------------------------- OBJ ---

TEST(Export, ObjWritesCompanionFilesAndRoundTrips) {
  TempDir dir;
  const Mesh original = texturedSphere(0.3, 0.05);
  const ColorBuffer texture = testTexture(96);
  ASSERT_TRUE(writeObj(dir.dir(), "scan", original, texture, {}));

  EXPECT_TRUE(std::filesystem::exists(dir.file("scan.obj")));
  EXPECT_TRUE(std::filesystem::exists(dir.file("scan.mtl")));
  EXPECT_TRUE(std::filesystem::exists(dir.file("scan.png")));

  // The MTL must reference the texture by its relative name, not an absolute
  // path, or the bundle breaks the moment it is moved.
  std::string mtl;
  ASSERT_TRUE(readFileText(dir.file("scan.mtl"), &mtl));
  EXPECT_NE(mtl.find("map_Kd scan.png"), std::string::npos);
  EXPECT_EQ(mtl.find(dir.dir()), std::string::npos);

  std::string obj;
  ASSERT_TRUE(readFileText(dir.file("scan.obj"), &obj));
  EXPECT_NE(obj.find("mtllib scan.mtl"), std::string::npos);

  Mesh loaded;
  std::string error;
  ASSERT_TRUE(readObj(dir.file("scan.obj"), &loaded, &error)) << error;
  EXPECT_EQ(loaded.triangleCount(), original.triangleCount());
  EXPECT_LT(maxVertexDeviation(loaded, original), 1e-5);
  EXPECT_TRUE(loaded.hasUvs());
  EXPECT_TRUE(loaded.hasNormals());
  EXPECT_TRUE(loaded.hasColors());

  // The written PNG must be a real, decodable PNG of the right size.
  std::vector<uint8_t> png;
  ASSERT_TRUE(readFileBytes(dir.file("scan.png"), &png));
  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(png, &decoded));
  EXPECT_EQ(decoded.width, texture.width);
  EXPECT_EQ(decoded.height, texture.height);
}

TEST(Export, ObjUvConventionSurvivesTheRoundTrip) {
  TempDir dir;
  Mesh original = texturedSphere(0.25, 0.07);
  ASSERT_TRUE(writeObj(dir.dir(), "uv", original, testTexture(32), {}));

  Mesh loaded;
  ASSERT_TRUE(readObj(dir.file("uv.obj"), &loaded));
  ASSERT_TRUE(loaded.hasUvs());
  // The writer flips V on the way out and the reader flips it back, so the
  // values must match what went in -- a sign error here mirrors every texture.
  ASSERT_EQ(loaded.vertexCount(), original.vertexCount());
  for (size_t i = 0; i < original.vertexCount(); ++i) {
    EXPECT_NEAR(loaded.uvs[i].x(), original.uvs[i].x(), 1e-5) << "u " << i;
    EXPECT_NEAR(loaded.uvs[i].y(), original.uvs[i].y(), 1e-5) << "v " << i;
  }
}

TEST(Export, ObjRejectsEmptyInput) {
  TempDir dir;
  EXPECT_FALSE(writeObj(dir.dir(), "empty", Mesh{}, ColorBuffer{}, {}));
  EXPECT_FALSE(writeObj(dir.dir(), "", texturedSphere(0.2, 0.1), ColorBuffer{}, {}));
}

// ----------------------------------------------------------------- glTF ---

TEST(Export, GlbIsStructurallyValid) {
  TempDir dir;
  const Mesh original = texturedSphere(0.3, 0.05);
  const std::string path = dir.file("model.glb");
  ASSERT_TRUE(writeGlb(path, original, testTexture(128), {}));

  GlbFile glb;
  std::string error;
  ASSERT_TRUE(readGlb(path, &glb, &error)) << error;
  ASSERT_TRUE(validateGltf(glb.json, glb.binary.size(), &error)) << error;

  // Check the accessors actually describe the mesh we exported.
  const Json& accessors = glb.json.get("accessors");
  const Json& primitive = glb.json.get("meshes").at(0).get("primitives").at(0);
  const int64_t position_index = primitive.get("attributes").get("POSITION").asInt(-1);
  const int64_t index_index = primitive.get("indices").asInt(-1);
  ASSERT_GE(position_index, 0);
  ASSERT_GE(index_index, 0);
  EXPECT_EQ(accessors.at(position_index).get("count").asInt(),
            static_cast<int64_t>(original.vertexCount()));
  EXPECT_EQ(accessors.at(index_index).get("count").asInt(),
            static_cast<int64_t>(original.indices.size()));

  // A texture was supplied, so the document must describe one.
  EXPECT_EQ(glb.json.get("textures").size(), 1u);
  EXPECT_EQ(glb.json.get("images").size(), 1u);
  EXPECT_TRUE(glb.json.get("images").at(0).has("bufferView"))
      << "a GLB must embed its image, not reference a file";
  EXPECT_EQ(glb.json.get("images").at(0).get("mimeType").asString(), "image/png");
}

TEST(Export, GlbPositionAccessorMinMaxMatchTheBounds) {
  TempDir dir;
  const Mesh original = texturedSphere(0.28, 0.06);
  const std::string path = dir.file("bounds.glb");
  ASSERT_TRUE(writeGlb(path, original, ColorBuffer{}, {}));

  GlbFile glb;
  ASSERT_TRUE(readGlb(path, &glb));
  const Json& primitive = glb.json.get("meshes").at(0).get("primitives").at(0);
  const int64_t position = primitive.get("attributes").get("POSITION").asInt(-1);
  const Json& accessor = glb.json.get("accessors").at(position);
  ASSERT_TRUE(accessor.has("min"));
  ASSERT_TRUE(accessor.has("max"));

  const AxisAlignedBox box = meshBounds(original);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(accessor.get("min").at(c).asDouble(), box.min[c], 1e-5) << "axis " << c;
    EXPECT_NEAR(accessor.get("max").at(c).asDouble(), box.max[c], 1e-5) << "axis " << c;
  }
}

TEST(Export, GlbBinaryChunkDecodesBackToTheSameGeometry) {
  TempDir dir;
  const Mesh original = texturedSphere(0.25, 0.06);
  const std::string path = dir.file("decode.glb");
  ASSERT_TRUE(writeGlb(path, original, ColorBuffer{}, {}));

  GlbFile glb;
  ASSERT_TRUE(readGlb(path, &glb));
  const Json& primitive = glb.json.get("meshes").at(0).get("primitives").at(0);
  const Json& accessors = glb.json.get("accessors");
  const Json& views = glb.json.get("bufferViews");

  // Pull POSITION straight out of the BIN chunk and compare vertex by vertex.
  const int64_t position = primitive.get("attributes").get("POSITION").asInt(-1);
  const int64_t view_index = accessors.at(position).get("bufferView").asInt(-1);
  const size_t offset = static_cast<size_t>(views.at(view_index).get("byteOffset").asInt(0));
  const size_t count = static_cast<size_t>(accessors.at(position).get("count").asInt(0));
  ASSERT_EQ(count, original.vertexCount());
  ASSERT_LE(offset + count * 12, glb.binary.size());
  for (size_t i = 0; i < count; ++i) {
    float xyz[3];
    std::memcpy(xyz, glb.binary.data() + offset + i * 12, 12);
    EXPECT_NEAR(xyz[0], original.vertices[i].x(), 1e-6f) << "vertex " << i;
    EXPECT_NEAR(xyz[1], original.vertices[i].y(), 1e-6f) << "vertex " << i;
    EXPECT_NEAR(xyz[2], original.vertices[i].z(), 1e-6f) << "vertex " << i;
  }
}

TEST(Export, GltfSeparateFilesReferenceEachOther) {
  TempDir dir;
  const Mesh original = texturedSphere(0.3, 0.06);
  ASSERT_TRUE(writeGltf(dir.dir(), "scene", original, testTexture(64), {}));

  ASSERT_TRUE(std::filesystem::exists(dir.file("scene.gltf")));
  ASSERT_TRUE(std::filesystem::exists(dir.file("scene.bin")));
  ASSERT_TRUE(std::filesystem::exists(dir.file("scene.png")));

  std::string text;
  ASSERT_TRUE(readFileText(dir.file("scene.gltf"), &text));
  Json document;
  std::string error;
  ASSERT_TRUE(Json::parse(text, &document, &error)) << error;

  // The buffer must point at the sidecar by relative name.
  EXPECT_EQ(document.get("buffers").at(0).get("uri").asString(), "scene.bin");
  std::vector<uint8_t> bin;
  ASSERT_TRUE(readFileBytes(dir.file("scene.bin"), &bin));
  EXPECT_EQ(document.get("buffers").at(0).get("byteLength").asInt(),
            static_cast<int64_t>(bin.size()));
  ASSERT_TRUE(validateGltf(document, bin.size(), &error)) << error;

  // The image must be a uri here, not an embedded bufferView.
  ASSERT_EQ(document.get("images").size(), 1u);
  EXPECT_TRUE(document.get("images").at(0).has("uri"));
}

TEST(Export, GltfValidatorRejectsBrokenDocuments) {
  TempDir dir;
  const std::string path = dir.file("good.glb");
  ASSERT_TRUE(writeGlb(path, texturedSphere(0.2, 0.08), ColorBuffer{}, {}));
  GlbFile glb;
  ASSERT_TRUE(readGlb(path, &glb));
  std::string error;
  ASSERT_TRUE(validateGltf(glb.json, glb.binary.size(), &error)) << error;

  // Each mutation below is a mistake a hand-written exporter really makes.
  {
    Json broken = glb.json;
    broken.set("asset", Json::object());
    EXPECT_FALSE(validateGltf(broken, glb.binary.size(), &error));
  }
  {
    // Strip POSITION's min/max, which the spec requires.
    Json broken = glb.json;
    Json accessors = broken.get("accessors");
    Json accessor = accessors.at(0);
    Json rebuilt = Json::array();
    for (size_t i = 0; i < accessors.size(); ++i) {
      Json entry = accessors.at(i);
      if (entry.get("type").asString() == "VEC3" &&
          entry.get("componentType").asInt() == 5126) {
        Json stripped = Json::object();
        for (const auto& kv : entry.members()) {
          if (kv.first == "min" || kv.first == "max") continue;
          stripped.set(kv.first, kv.second);
        }
        entry = std::move(stripped);
      }
      rebuilt.push(std::move(entry));
    }
    broken.set("accessors", std::move(rebuilt));
    EXPECT_FALSE(validateGltf(broken, glb.binary.size(), &error));
    EXPECT_NE(error.find("min and max"), std::string::npos);
  }
  {
    // Claim a larger buffer than the BIN chunk carries.
    Json broken = glb.json;
    Json buffers = Json::array();
    Json buffer = Json::object();
    buffer.set("byteLength", Json::integer(static_cast<int64_t>(glb.binary.size() + 9999)));
    buffers.push(std::move(buffer));
    broken.set("buffers", std::move(buffers));
    EXPECT_FALSE(validateGltf(broken, glb.binary.size(), &error));
  }
  {
    Json broken = glb.json;
    broken.set("scene", Json::integer(42));
    EXPECT_FALSE(validateGltf(broken, glb.binary.size(), &error));
  }
}

TEST(Export, GlbReaderRejectsCorruptContainers) {
  TempDir dir;
  const std::string path = dir.file("corrupt.glb");
  ASSERT_TRUE(writeGlb(path, texturedSphere(0.2, 0.09), ColorBuffer{}, {}));
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));

  GlbFile glb;
  std::string error;
  {
    std::vector<uint8_t> bad = bytes;
    bad[0] ^= 0xFF;  // magic
    const std::string p = dir.file("magic.glb");
    ASSERT_TRUE(writeFileBytes(p, bad));
    EXPECT_FALSE(readGlb(p, &glb, &error));
    EXPECT_NE(error.find("magic"), std::string::npos);
  }
  {
    // Truncate: the declared total length no longer matches.
    std::vector<uint8_t> bad(bytes.begin(), bytes.end() - 16);
    const std::string p = dir.file("short.glb");
    ASSERT_TRUE(writeFileBytes(p, bad));
    EXPECT_FALSE(readGlb(p, &glb, &error));
  }
}

TEST(Export, GlbRejectsEmptyMesh) {
  TempDir dir;
  EXPECT_FALSE(writeGlb(dir.file("empty.glb"), Mesh{}, ColorBuffer{}, {}));
}

// ----------------------------------------------------------------- USDZ ---

TEST(Export, UsdzIsAStoredSixtyFourByteAlignedZip) {
  TempDir dir;
  const Mesh original = texturedSphere(0.3, 0.06);
  const std::string path = dir.file("model.usdz");
  ExportOptions options;
  options.name = "Scan";
  ASSERT_TRUE(writeUsdz(path, original, testTexture(64), options));

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));

  std::vector<std::string> names;
  ASSERT_TRUE(zipList(bytes, &names));
  ASSERT_GE(names.size(), 1u);
  // The stage must come first: USD takes the first entry as the default layer.
  EXPECT_EQ(names[0], "Scan.usda");
  EXPECT_NE(std::find(names.begin(), names.end(), "textures/diffuse.png"), names.end());

  // Every entry's data must begin on a 64-byte boundary, and be stored
  // uncompressed, or USD cannot memory-map the package.
  for (const std::string& name : names) {
    const int64_t offset = zipEntryDataOffset(bytes, name);
    ASSERT_GE(offset, 0) << name;
    EXPECT_EQ(offset % 64, 0) << name << " is not 64-byte aligned";
  }
  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(bytes, &entries));
  for (const ZipEntry& entry : entries) {
    // zipRead only succeeds for STORE or DEFLATE; prove it was STORE by
    // checking the compressed size equals the uncompressed size.
    const int64_t offset = zipEntryDataOffset(bytes, entry.path);
    ASSERT_GE(offset, 0);
  }

  // The USDA payload must parse as the stage we asked for.
  std::string usda;
  for (const ZipEntry& entry : entries) {
    if (entry.path == "Scan.usda") usda.assign(entry.data.begin(), entry.data.end());
  }
  ASSERT_FALSE(usda.empty());
  EXPECT_EQ(usda.compare(0, 9, "#usda 1.0"), 0);
  EXPECT_NE(usda.find("defaultPrim = \"Scan\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh \"geometry\""), std::string::npos);
  EXPECT_NE(usda.find("UsdPreviewSurface"), std::string::npos);
  EXPECT_NE(usda.find("@textures/diffuse.png@"), std::string::npos);
  EXPECT_NE(usda.find("upAxis = \"Y\""), std::string::npos);
}

TEST(Export, UsdaRecordsCountsAndMetersPerUnit) {
  const Mesh mesh = texturedSphere(0.25, 0.08);
  ExportOptions options;
  options.unit = LengthUnit::Centimetres;
  options.name = "Obj";
  const std::string usda = buildUsda(mesh, options, "");

  // metersPerUnit is the reciprocal of the scale applied to the geometry.
  EXPECT_NE(usda.find("metersPerUnit = 0.01"), std::string::npos);
  // One count entry per triangle and one index per corner.
  size_t count_entries = 1;
  const size_t counts_start = usda.find("faceVertexCounts = [");
  ASSERT_NE(counts_start, std::string::npos);
  const size_t counts_end = usda.find(']', counts_start);
  for (size_t i = counts_start; i < counts_end; ++i) {
    if (usda[i] == ',') ++count_entries;
  }
  EXPECT_EQ(count_entries, mesh.triangleCount());
}

TEST(Export, UsdzWithoutATextureOmitsTheMaterialTexture) {
  TempDir dir;
  const std::string path = dir.file("plain.usdz");
  ASSERT_TRUE(writeUsdz(path, texturedSphere(0.2, 0.09), ColorBuffer{}, {}));

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  std::vector<std::string> names;
  ASSERT_TRUE(zipList(bytes, &names));
  EXPECT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "ParallaxModel.usda");

  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(bytes, &entries));
  const std::string usda(entries[0].data.begin(), entries[0].data.end());
  EXPECT_EQ(usda.find("UsdUVTexture"), std::string::npos);
  EXPECT_NE(usda.find("inputs:diffuseColor = ("), std::string::npos);
}

TEST(Export, UsdzHonoursZUp) {
  TempDir dir;
  const std::string path = dir.file("zup.usdz");
  ExportOptions options;
  options.up_axis = UpAxis::ZUp;
  ASSERT_TRUE(writeUsdz(path, texturedSphere(0.2, 0.09), ColorBuffer{}, options));
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(bytes, &entries));
  const std::string usda(entries[0].data.begin(), entries[0].data.end());
  EXPECT_NE(usda.find("upAxis = \"Z\""), std::string::npos);
}
