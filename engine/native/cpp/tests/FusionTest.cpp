#include "parallax/fusion/TsdfVolume.h"

#include <gtest/gtest.h>

#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// Analytic SDF of a sphere, used to drive the extractor directly.
auto sphereField(const Vec3& centre, double radius) {
  return [centre, radius](const Vec3& p, float* value, std::array<uint8_t, 3>* color) {
    *value = static_cast<float>((p - centre).norm() - radius);
    if (color) *color = {180, 120, 60};
    return true;
  };
}

/// Integrate a full orbit of a scene into a volume.
TsdfVolume fuseScene(const SyntheticScene& scene, const Vec3& target, double radius,
                     double elevation, int views, const TsdfConfig& config,
                     const CameraIntrinsics& k) {
  TsdfVolume volume(config);
  const std::vector<Pose> poses =
      SyntheticScene::orbitPoses(target, radius, elevation, views);
  for (const Pose& pose : poses) {
    const RenderResult frame = scene.render(k, pose, true, true);
    volume.integrate(frame.depth, frame.color, k, pose);
  }
  return volume;
}

/// Largest absolute deviation of mesh vertices from an analytic sphere.
double sphereRadialError(const Mesh& mesh, const Vec3& centre, double radius) {
  double worst = 0.0;
  for (const Vec3f& v : mesh.vertices) {
    const Vec3 p(v.x(), v.y(), v.z());
    worst = std::max(worst, std::abs((p - centre).norm() - radius));
  }
  return worst;
}

}  // namespace

// ------------------------------------------------- marching tetrahedra ------

TEST(MarchingTetrahedra, ExtractsAClosedSphere) {
  const Vec3 centre(0.0, 0.0, 0.0);
  const double radius = 0.5;
  AxisAlignedBox bounds;
  bounds.expand(centre - Vec3::Constant(radius * 1.4));
  bounds.expand(centre + Vec3::Constant(radius * 1.4));

  const Mesh mesh = marchingTetrahedra(bounds, 0.04, sphereField(centre, radius));
  ASSERT_FALSE(mesh.empty());
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;

  // A closed surface: every edge shared by exactly two triangles.
  EXPECT_TRUE(isWatertight(mesh));
  // A sphere is genus 0, so V - E + F = 2.
  EXPECT_EQ(eulerCharacteristic(mesh), 2);
  // One connected component.
  std::vector<int> labels;
  EXPECT_EQ(labelConnectedComponents(mesh, &labels), 1);
}

TEST(MarchingTetrahedra, GeometryConvergesWithResolution) {
  const Vec3 centre(0.1, -0.05, 0.2);
  const double radius = 0.4;
  AxisAlignedBox bounds;
  bounds.expand(centre - Vec3::Constant(radius * 1.5));
  bounds.expand(centre + Vec3::Constant(radius * 1.5));

  double previous_error = 1e9;
  for (double cell : {0.08, 0.04, 0.02}) {
    const Mesh mesh = marchingTetrahedra(bounds, cell, sphereField(centre, radius));
    ASSERT_FALSE(mesh.empty()) << "cell " << cell;
    const double error = sphereRadialError(mesh, centre, radius);
    // Linear interpolation along edges gives error that falls with the cell size.
    EXPECT_LT(error, previous_error) << "cell " << cell;
    EXPECT_LT(error, cell * 0.35) << "cell " << cell;
    previous_error = error;

    // Surface area and volume should approach the analytic values.
    const double expected_area = 4.0 * kPi * radius * radius;
    const double expected_volume = 4.0 / 3.0 * kPi * radius * radius * radius;
    EXPECT_NEAR(meshSurfaceArea(mesh), expected_area, expected_area * 0.08);
    EXPECT_NEAR(std::abs(meshSignedVolume(mesh)), expected_volume, expected_volume * 0.08);
  }
}

TEST(MarchingTetrahedra, OrientsNormalsOutward) {
  const Vec3 centre = Vec3::Zero();
  const double radius = 0.45;
  AxisAlignedBox bounds;
  bounds.expand(centre - Vec3::Constant(0.7));
  bounds.expand(centre + Vec3::Constant(0.7));
  const Mesh mesh = marchingTetrahedra(bounds, 0.03, sphereField(centre, radius));
  ASSERT_TRUE(mesh.hasNormals());

  // Winding must give a positive enclosed volume, and vertex normals must point
  // away from the centre.
  EXPECT_GT(meshSignedVolume(mesh), 0.0);
  int outward = 0;
  for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    const Vec3 p(mesh.vertices[i].x(), mesh.vertices[i].y(), mesh.vertices[i].z());
    const Vec3 n(mesh.normals[i].x(), mesh.normals[i].y(), mesh.normals[i].z());
    if (p.normalized().dot(n) > 0.0) ++outward;
  }
  EXPECT_GT(static_cast<double>(outward) / mesh.vertices.size(), 0.99);
}

TEST(MarchingTetrahedra, ProducesNothingForAFieldWithoutACrossing) {
  AxisAlignedBox bounds;
  bounds.expand(Vec3(-1, -1, -1));
  bounds.expand(Vec3(1, 1, 1));
  const Mesh mesh = marchingTetrahedra(bounds, 0.1, [](const Vec3&, float* v,
                                                       std::array<uint8_t, 3>*) {
    *v = 1.0f;  // everywhere outside
    return true;
  });
  EXPECT_TRUE(mesh.empty());
}

TEST(MarchingTetrahedra, SkipsUnobservedRegions) {
  // Only the +x half is observed; no surface may be invented in the other half.
  AxisAlignedBox bounds;
  bounds.expand(Vec3(-1, -1, -1));
  bounds.expand(Vec3(1, 1, 1));
  const Mesh mesh =
      marchingTetrahedra(bounds, 0.05, [](const Vec3& p, float* v, std::array<uint8_t, 3>*) {
        if (p.x() < 0.0) return false;
        *v = static_cast<float>(p.norm() - 0.5);
        return true;
      });
  for (const Vec3f& vtx : mesh.vertices) {
    EXPECT_GE(vtx.x(), -0.06f) << "surface leaked into unobserved space";
  }
}

TEST(MarchingTetrahedra, RejectsDegenerateArguments) {
  AxisAlignedBox invalid;
  EXPECT_TRUE(marchingTetrahedra(invalid, 0.1, sphereField(Vec3::Zero(), 1.0)).empty());
  AxisAlignedBox tiny;
  tiny.expand(Vec3::Zero());
  tiny.expand(Vec3::Constant(0.001));
  EXPECT_TRUE(marchingTetrahedra(tiny, 0.0, sphereField(Vec3::Zero(), 1.0)).empty());
  EXPECT_TRUE(marchingTetrahedra(tiny, -1.0, sphereField(Vec3::Zero(), 1.0)).empty());
}

// --------------------------------------------------------- tsdf volume ------

TEST(Tsdf, EnforcesAMinimumTruncationWidth) {
  TsdfConfig config;
  config.voxel_size = 0.01f;
  config.truncation = 0.001f;  // narrower than a voxel
  const TsdfVolume volume(config);
  EXPECT_GE(volume.config().truncation, config.voxel_size * 2.0f);
}

TEST(Tsdf, RejectsMismatchedOrEmptyInput) {
  TsdfVolume volume;
  const CameraIntrinsics k = phoneIntrinsics(64, 48);
  EXPECT_FALSE(volume.integrate(DepthMap{}, ColorBuffer{}, k, Pose::Identity()));
  DepthMap wrong_size(32, 32);
  EXPECT_FALSE(volume.integrate(wrong_size, ColorBuffer{}, k, Pose::Identity()));
  DepthMap right_size(64, 48);
  EXPECT_FALSE(volume.integrate(right_size, ColorBuffer{}, CameraIntrinsics{}, Pose::Identity()));
}

TEST(Tsdf, IntegratesASinglePlaneWithCorrectSign) {
  // A plane at z = 1 filling the view. The TSDF must be positive in front of it
  // (towards the camera) and negative behind.
  const CameraIntrinsics k = phoneIntrinsics(80, 60);
  DepthMap depth(80, 60);
  for (int y = 0; y < 60; ++y) {
    for (int x = 0; x < 80; ++x) depth.at(x, y) = 1.0f;
  }
  TsdfConfig config;
  config.voxel_size = 0.02f;
  config.truncation = 0.1f;
  config.weight_by_incidence = false;
  TsdfVolume volume(config);
  ASSERT_TRUE(volume.integrate(depth, ColorBuffer{}, k, Pose::Identity()));
  EXPECT_GT(volume.stats().observed_voxels, 0u);

  float value = 0.f;
  // In front of the surface (closer to the camera) -> positive.
  ASSERT_TRUE(volume.sampleDistance(Vec3(0, 0, 0.95), &value));
  EXPECT_GT(value, 0.0f);
  // Behind the surface -> negative.
  ASSERT_TRUE(volume.sampleDistance(Vec3(0, 0, 1.05), &value));
  EXPECT_LT(value, 0.0f);
  // On the surface -> near zero.
  ASSERT_TRUE(volume.sampleDistance(Vec3(0, 0, 1.0), &value));
  EXPECT_NEAR(value, 0.0f, config.voxel_size);
}

TEST(Tsdf, SampleFailsOutsideTheObservedRegion) {
  const CameraIntrinsics k = phoneIntrinsics(64, 48);
  DepthMap depth(64, 48);
  for (auto& d : depth.depth) d = 1.0f;
  TsdfVolume volume;
  ASSERT_TRUE(volume.integrate(depth, ColorBuffer{}, k, Pose::Identity()));
  float value = 0.f;
  EXPECT_FALSE(volume.sampleDistance(Vec3(50.0, 50.0, 50.0), &value));
}

TEST(Tsdf, FusesAnOrbitIntoAnAccurateMesh) {
  // The headline accuracy test: fuse a real orbit of a synthetic scene and
  // measure the reconstructed sphere against its known radius.
  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.center = Vec3(0, 0, 0);
  ball.radius = 0.25;
  ball.texture = Texture::Noise;
  ball.texture_scale = 20.0;
  scene.primitives.push_back(ball);

  TsdfConfig config;
  config.voxel_size = 0.006f;
  config.truncation = 0.02f;
  config.max_depth = 3.0f;
  const CameraIntrinsics k = phoneIntrinsics(240, 180);

  // A full orbit plus two elevations, so the poles are covered too.
  TsdfVolume volume(config);
  for (double elevation : {-0.35, 0.0, 0.35}) {
    for (const Pose& pose : SyntheticScene::orbitPoses(ball.center, 0.75, elevation, 24)) {
      const RenderResult frame = scene.render(k, pose, true, true);
      ASSERT_TRUE(volume.integrate(frame.depth, frame.color, k, pose));
    }
  }
  EXPECT_EQ(volume.stats().integrated_frames, 72u);
  EXPECT_GT(volume.stats().observed_voxels, 1000u);

  Mesh mesh = volume.extractMesh();
  ASSERT_FALSE(mesh.empty());
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;
  removeSmallComponents(&mesh, 32, 0.05);

  // Reconstructed radius should be within a couple of voxels of the truth.
  double mean_radius = 0.0;
  for (const Vec3f& v : mesh.vertices) {
    mean_radius += (Vec3(v.x(), v.y(), v.z()) - ball.center).norm();
  }
  mean_radius /= mesh.vertices.size();
  EXPECT_NEAR(mean_radius, ball.radius, 3.0 * config.voxel_size)
      << "mean reconstructed radius " << mean_radius;
  EXPECT_LT(sphereRadialError(mesh, ball.center, ball.radius), 8.0 * config.voxel_size);

  // Colour must have been carried through fusion.
  EXPECT_TRUE(mesh.hasColors());
}

TEST(Tsdf, ExtractsAPointCloudOnTheSurface) {
  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.3;
  ball.texture = Texture::Noise;
  scene.primitives.push_back(ball);

  TsdfConfig config;
  config.voxel_size = 0.01f;
  config.truncation = 0.035f;
  const CameraIntrinsics k = phoneIntrinsics(200, 150);
  const TsdfVolume volume =
      fuseScene(scene, ball.center, 0.9, 0.0, 28, config, k);

  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  volume.extractPointCloud(&points, &normals, &colors);
  ASSERT_GT(points.size(), 500u);
  EXPECT_EQ(points.size(), normals.size());
  EXPECT_EQ(points.size(), colors.size());

  // Points must lie on the sphere, and normals must point outward.
  double worst = 0.0;
  int outward = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    const Vec3 p(points[i].x(), points[i].y(), points[i].z());
    worst = std::max(worst, std::abs(p.norm() - ball.radius));
    const Vec3 n(normals[i].x(), normals[i].y(), normals[i].z());
    EXPECT_NEAR(n.norm(), 1.0, 1e-4);
    if (p.normalized().dot(n) > 0.0) ++outward;
  }
  EXPECT_LT(worst, 4.0 * config.voxel_size);
  EXPECT_GT(static_cast<double>(outward) / points.size(), 0.95);
}

TEST(Tsdf, RespectsTheBlockBudget) {
  SyntheticScene scene = SyntheticScene::room(6.0, 5.0, 2.6);
  TsdfConfig config;
  config.voxel_size = 0.02f;
  config.truncation = 0.07f;
  config.max_blocks = 40;  // deliberately tiny
  config.max_depth = 8.0f;
  TsdfVolume volume(config);

  const CameraIntrinsics k = phoneIntrinsics(160, 120);
  for (const Pose& pose : SyntheticScene::orbitPoses(Vec3(0, 1.2, 0), 1.2, 0.0, 8)) {
    volume.integrate(scene.render(k, pose, true, true).depth, ColorBuffer{}, k, pose);
  }
  EXPECT_LE(volume.stats().allocated_blocks, 40u);
  EXPECT_GT(volume.stats().block_allocation_failures, 0u)
      << "budget should have been hit and reported";
  // It must still produce a usable (if partial) result rather than failing.
  EXPECT_GT(volume.stats().observed_voxels, 0u);
}

TEST(Tsdf, SerializationRoundTripsExactly) {
  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.28;
  ball.texture = Texture::Checker;
  scene.primitives.push_back(ball);

  TsdfConfig config;
  config.voxel_size = 0.015f;
  config.truncation = 0.05f;
  const CameraIntrinsics k = phoneIntrinsics(128, 96);
  const TsdfVolume original = fuseScene(scene, ball.center, 0.85, 0.1, 10, config, k);

  const std::vector<uint8_t> bytes = original.serialize();
  ASSERT_GT(bytes.size(), 64u);

  TsdfVolume restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_EQ(restored.stats().allocated_blocks, original.stats().allocated_blocks);
  EXPECT_EQ(restored.stats().observed_voxels, original.stats().observed_voxels);
  EXPECT_FLOAT_EQ(restored.config().voxel_size, config.voxel_size);

  // Sampling must agree bit for bit, which is what makes checkpoint/resume safe.
  Rng rng(4);
  int compared = 0;
  for (int i = 0; i < 4000 && compared < 200; ++i) {
    const Vec3 p = rng.unitVector() * rng.uniform(0.2, 0.4);
    float a = 0.f, b = 0.f;
    const bool ok_a = original.sampleDistance(p, &a);
    const bool ok_b = restored.sampleDistance(p, &b);
    EXPECT_EQ(ok_a, ok_b);
    if (ok_a) {
      EXPECT_FLOAT_EQ(a, b);
      ++compared;
    }
  }
  EXPECT_GT(compared, 50);
}

TEST(Tsdf, DeserializeRejectsCorruptInput) {
  TsdfVolume volume;
  EXPECT_FALSE(volume.deserialize({}));
  EXPECT_FALSE(volume.deserialize({1, 2, 3, 4, 5, 6, 7, 8}));

  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.2;
  scene.primitives.push_back(ball);
  TsdfConfig config;
  config.voxel_size = 0.02f;
  const TsdfVolume good =
      fuseScene(scene, ball.center, 0.7, 0.0, 6, config, phoneIntrinsics(96, 72));
  std::vector<uint8_t> bytes = good.serialize();

  // Truncate mid-block: must be detected, not read out of bounds.
  std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
  TsdfVolume target;
  EXPECT_FALSE(target.deserialize(truncated));
  EXPECT_EQ(target.stats().allocated_blocks, 0u) << "failed load must leave nothing behind";

  // Corrupt the magic number.
  bytes[0] ^= 0xFF;
  EXPECT_FALSE(target.deserialize(bytes));
}

TEST(Tsdf, ClearResetsEverything) {
  SyntheticScene scene;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.25;
  scene.primitives.push_back(ball);
  TsdfConfig config;
  config.voxel_size = 0.02f;
  TsdfVolume volume(config);
  const CameraIntrinsics k = phoneIntrinsics(96, 72);
  for (const Pose& pose : SyntheticScene::orbitPoses(ball.center, 0.8, 0.0, 6)) {
    volume.integrate(scene.render(k, pose, true, true).depth, ColorBuffer{}, k, pose);
  }
  ASSERT_GT(volume.stats().observed_voxels, 0u);
  volume.clear();
  EXPECT_EQ(volume.stats().allocated_blocks, 0u);
  EXPECT_EQ(volume.stats().observed_voxels, 0u);
  EXPECT_TRUE(volume.extractMesh().empty());
}
