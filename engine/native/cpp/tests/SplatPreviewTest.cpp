#include "parallax/splat/SplatPreview.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <set>

#include "parallax/exporters/MeshExport.h"
#include "parallax/io/Json.h"
#include "parallax/io/MeshReader.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A patch of points on the y = 0 plane, on a regular grid.
void flatGrid(int side, float spacing, std::vector<Vec3f>* points, std::vector<Vec3f>* normals,
              std::vector<std::array<uint8_t, 3>>* colors) {
  for (int z = 0; z < side; ++z) {
    for (int x = 0; x < side; ++x) {
      points->emplace_back(x * spacing, 0.f, z * spacing);
      normals->emplace_back(0.f, 1.f, 0.f);
      colors->push_back({static_cast<uint8_t>(x * 7 % 256),
                         static_cast<uint8_t>(z * 11 % 256), 128});
    }
  }
}

GaussianCloud smallCloud(size_t count, float opacity_logit = 2.f) {
  GaussianCloud cloud;
  cloud.sh_degree = 0;
  Rng rng(99);
  for (size_t i = 0; i < count; ++i) {
    Gaussian g;
    g.position = Vec3f(static_cast<float>(rng.uniform(-1.0, 1.0)),
                       static_cast<float>(rng.uniform(-1.0, 1.0)),
                       static_cast<float>(rng.uniform(-1.0, 1.0)));
    // Deliberately anisotropic, so "drop the thinnest axis" has something to do.
    g.log_scale = Vec3f(std::log(0.05f), std::log(0.02f), std::log(0.004f));
    g.rotation = Eigen::Quaternionf::Identity();
    g.opacity_logit = opacity_logit;
    g.dc = {0.4f, -0.2f, 0.1f};
    cloud.gaussians.push_back(g);
  }
  return cloud;
}

}  // namespace

TEST(SplatPreview, AnEmptyCloudProducesNoGeometryRatherThanAnInvalidMesh) {
  EXPECT_TRUE(buildPointCloudPreview({}, {}, {}).empty());
  EXPECT_TRUE(buildSplatPreview(GaussianCloud{}).empty());
}

TEST(SplatPreview, EveryPointBecomesADiscAtItsOwnPosition) {
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  flatGrid(8, 0.05f, &points, &normals, &colors);

  PointPreviewOptions options;
  options.segments = 3;
  const Mesh mesh = buildPointCloudPreview(points, normals, colors, options);

  std::string reason;
  ASSERT_TRUE(mesh.isValid(&reason)) << reason;
  EXPECT_EQ(points.size() * 3, mesh.vertexCount());
  EXPECT_EQ(points.size(), mesh.triangleCount());
  EXPECT_TRUE(mesh.hasColors());
  EXPECT_TRUE(mesh.hasNormals());
  EXPECT_TRUE(mesh.hasUvs());

  // Each disc's vertices average back to the point it came from.
  for (size_t p = 0; p < points.size(); ++p) {
    Vec3f centre = Vec3f::Zero();
    for (int c = 0; c < 3; ++c) centre += mesh.vertices[p * 3 + c];
    centre /= 3.f;
    EXPECT_LT((centre - points[p]).norm(), 1e-5f) << "point " << p;
  }
}

TEST(SplatPreview, DiscsFaceTheSurfaceAndAreSizedFromThePointSpacing) {
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  flatGrid(10, 0.04f, &points, &normals, &colors);
  const Mesh mesh = buildPointCloudPreview(points, normals, colors);

  // A disc on a flat floor must lie in the floor, not stick out of it.
  for (size_t v = 0; v < mesh.vertexCount(); ++v) {
    EXPECT_NEAR(0.f, mesh.vertices[v].y(), 1e-5f);
    EXPECT_NEAR(1.f, mesh.normals[v].y(), 1e-5f);
  }

  // The radius should be comparable to the spacing: much smaller leaves holes
  // and much larger turns a surface into a blob.
  float furthest = 0.f;
  for (int c = 0; c < 3; ++c) furthest = std::max(furthest, (mesh.vertices[c] - points[0]).norm());
  EXPECT_GT(furthest, 0.04f * 0.3f);
  EXPECT_LT(furthest, 0.04f * 1.5f);
}

TEST(SplatPreview, ASparseCloudGetsLargerDiscsThanADenseOne) {
  std::vector<Vec3f> dense_points, dense_normals, sparse_points, sparse_normals;
  std::vector<std::array<uint8_t, 3>> dense_colors, sparse_colors;
  flatGrid(10, 0.02f, &dense_points, &dense_normals, &dense_colors);
  flatGrid(10, 0.20f, &sparse_points, &sparse_normals, &sparse_colors);

  const auto radiusOf = [](const Mesh& mesh, const Vec3f& centre) {
    float furthest = 0.f;
    for (int c = 0; c < 3; ++c) furthest = std::max(furthest, (mesh.vertices[c] - centre).norm());
    return furthest;
  };
  const float dense =
      radiusOf(buildPointCloudPreview(dense_points, dense_normals, dense_colors), dense_points[0]);
  const float sparse = radiusOf(
      buildPointCloudPreview(sparse_points, sparse_normals, sparse_colors), sparse_points[0]);
  EXPECT_GT(sparse, dense * 4.f);
}

TEST(SplatPreview, APointWithNoNormalStillGetsADisc) {
  const std::vector<Vec3f> points{Vec3f(0.f, 0.f, 0.f)};
  // No normals at all, and a zero-length one, are both ordinary in a scan.
  const Mesh without = buildPointCloudPreview(points, {}, {});
  EXPECT_EQ(1u, without.triangleCount());
  const Mesh degenerate = buildPointCloudPreview(points, {Vec3f::Zero()}, {});
  EXPECT_EQ(1u, degenerate.triangleCount());
  std::string reason;
  EXPECT_TRUE(degenerate.isValid(&reason)) << reason;
  for (const Vec3f& vertex : degenerate.vertices) {
    EXPECT_TRUE(std::isfinite(vertex.x()));
    EXPECT_TRUE(std::isfinite(vertex.y()));
    EXPECT_TRUE(std::isfinite(vertex.z()));
  }
}

TEST(SplatPreview, ThePointCapSamplesEvenlyRatherThanTruncating) {
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  flatGrid(40, 0.05f, &points, &normals, &colors);  // 1600 points

  PointPreviewOptions options;
  options.segments = 3;
  options.max_points = 200;
  const Mesh mesh = buildPointCloudPreview(points, normals, colors, options);
  EXPECT_LE(mesh.triangleCount(), 200u);
  EXPECT_GT(mesh.triangleCount(), 100u);

  // Evenly sampled means the kept points still span the whole patch; taking
  // the first two hundred would cover only the first few rows.
  float maxZ = 0.f;
  for (const Vec3f& vertex : mesh.vertices) maxZ = std::max(maxZ, vertex.z());
  EXPECT_GT(maxZ, 39 * 0.05f * 0.9f);
}

TEST(SplatPreview, EachGaussianBecomesAQuadSpanningItsWidestAxes) {
  const GaussianCloud cloud = smallCloud(16);
  const Mesh mesh = buildSplatPreview(cloud);

  std::string reason;
  ASSERT_TRUE(mesh.isValid(&reason)) << reason;
  EXPECT_EQ(cloud.size() * 4, mesh.vertexCount());
  EXPECT_EQ(cloud.size() * 2, mesh.triangleCount());

  // The quads are built from the two larger scales (0.05 and 0.02), so their
  // half-extents should match those and not the 0.004 one that was dropped.
  SplatPreviewOptions options;
  const Mesh sized = buildSplatPreview(cloud, options);
  const Vec3f a = sized.vertices[0];
  const Vec3f b = sized.vertices[1];
  const Vec3f d = sized.vertices[3];
  const float width = (b - a).norm() * 0.5f;
  const float height = (d - a).norm() * 0.5f;
  EXPECT_NEAR(0.05f * options.extent_sigma, std::max(width, height), 1e-3f);
  EXPECT_NEAR(0.02f * options.extent_sigma, std::min(width, height), 1e-3f);
}

TEST(SplatPreview, QuadUvsCoverTheFalloffTexture) {
  const Mesh mesh = buildSplatPreview(smallCloud(4));
  ASSERT_TRUE(mesh.hasUvs());
  std::set<std::pair<int, int>> corners;
  for (size_t v = 0; v < 4; ++v) {
    EXPECT_GE(mesh.uvs[v].x(), 0.f);
    EXPECT_LE(mesh.uvs[v].x(), 1.f);
    corners.insert({static_cast<int>(std::lround(mesh.uvs[v].x())),
                    static_cast<int>(std::lround(mesh.uvs[v].y()))});
  }
  // All four corners of the texture, so the whole ramp is used.
  EXPECT_EQ(4u, corners.size());
}

TEST(SplatPreview, TransparentGaussiansAreLeftOut) {
  // sigmoid(-6) is about 0.0025, well under the default threshold.
  const Mesh mesh = buildSplatPreview(smallCloud(8, -6.f));
  EXPECT_TRUE(mesh.empty());
}

TEST(SplatPreview, TheGaussianCapKeepsTheMostOpaque) {
  GaussianCloud cloud = smallCloud(32);
  for (size_t i = 0; i < cloud.size(); ++i) {
    cloud.gaussians[i].opacity_logit = static_cast<float>(i) * 0.2f - 2.f;
  }
  SplatPreviewOptions options;
  options.max_gaussians = 8;
  const Mesh mesh = buildSplatPreview(cloud, options);
  EXPECT_EQ(8u * 4u, mesh.vertexCount());

  // Those eight must be the eight most opaque, which are the last indices.
  float lowest_kept = 1.f;
  for (size_t q = 0; q < 8; ++q) {
    // Reconstruct the gaussian each quad came from by position.
    const Vec3f centre =
        (mesh.vertices[q * 4] + mesh.vertices[q * 4 + 2]) * 0.5f;
    float best = 1.f;
    for (const Gaussian& gaussian : cloud.gaussians) {
      if ((gaussian.position - centre).norm() < 1e-4f) best = gaussianOpacity(gaussian);
    }
    lowest_kept = std::min(lowest_kept, best);
  }
  EXPECT_GT(lowest_kept, gaussianOpacity(cloud.gaussians.front()));
}

TEST(SplatPreview, TheFalloffTextureIsAGaussianRampFromTheCentre) {
  const ColorBuffer texture = splatFalloffTexture(64, 2.5f);
  ASSERT_EQ(64, texture.width);
  ASSERT_EQ(64, texture.height);

  uint8_t centre[3], edge[3], corner[3];
  texture.get(32, 32, centre);
  texture.get(63, 32, edge);
  texture.get(63, 63, corner);
  EXPECT_GT(centre[0], 240);
  EXPECT_LT(edge[0], 60);
  EXPECT_LT(corner[0], edge[0]);
  // Greyscale, so it can ride in a base-colour texture and modulate alpha.
  texture.get(20, 40, centre);
  EXPECT_EQ(centre[0], centre[1]);
  EXPECT_EQ(centre[1], centre[2]);

  // Monotonically decreasing away from the middle along a row.
  uint8_t previous[3];
  texture.get(32, 32, previous);
  for (int x = 33; x < 64; ++x) {
    uint8_t value[3];
    texture.get(x, 32, value);
    EXPECT_LE(value[0], previous[0]) << "x " << x;
    previous[0] = value[0];
  }
}

TEST(SplatPreview, PreviewGeometryRoundTripsThroughTheGlbWriterTheViewerUses) {
  // The viewer has exactly one loader, so what matters is that this geometry
  // survives the path it will actually take.
  const Mesh mesh = buildSplatPreview(smallCloud(12));
  ASSERT_FALSE(mesh.empty());

  const auto directory = std::filesystem::temp_directory_path() / "parallax_preview_glb";
  std::filesystem::create_directories(directory);
  const std::string path = (directory / "preview.glb").string();

  ExportOptions options;
  options.name = "SplatPreview";
  options.unlit = true;
  options.blend = true;
  ASSERT_TRUE(writeGlb(path, mesh, splatFalloffTexture(32), options));

  GlbFile glb;
  std::string error;
  ASSERT_TRUE(readGlb(path, &glb, &error)) << error;
  ASSERT_TRUE(validateGltf(glb.json, glb.binary.size(), &error)) << error;

  // One primitive, with the colour and texture coordinates the falloff needs.
  const Json& primitive = glb.json.get("meshes").at(0).get("primitives").at(0);
  const Json& attributes = primitive.get("attributes");
  EXPECT_TRUE(attributes.has("POSITION"));
  EXPECT_TRUE(attributes.has("COLOR_0"));
  EXPECT_TRUE(attributes.has("TEXCOORD_0"));
  const int position_accessor = static_cast<int>(attributes.get("POSITION").asInt());
  EXPECT_EQ(static_cast<int64_t>(mesh.vertexCount()),
            glb.json.get("accessors").at(position_accessor).get("count").asInt());

  // Unlit and blended, which is what gives the preview its colours and its
  // soft edge without a shader of its own.
  const Json& material = glb.json.get("materials").at(0);
  EXPECT_EQ("BLEND", material.get("alphaMode").asString());
  EXPECT_TRUE(material.get("extensions").has("KHR_materials_unlit"));
  EXPECT_EQ("KHR_materials_unlit", glb.json.get("extensionsUsed").at(0).asString());
  std::filesystem::remove_all(directory);
}
