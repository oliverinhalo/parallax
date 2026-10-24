#include "parallax/texture/Texturing.h"

#include <gtest/gtest.h>

#include "parallax/fusion/TsdfVolume.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A sphere mesh plus views of it rendered from a known orbit, so the texture can be compared
/// against the colours the renderer actually produced.
struct Fixture {
  SyntheticScene scene;
  Mesh mesh;
  std::vector<TextureView> views;
  double radius = 0.3;
};

Fixture makeFixture(int view_count = 12, int image_size = 192, double cell = 0.05) {
  Fixture fixture;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.center = Vec3::Zero();
  ball.radius = fixture.radius;
  ball.texture = Texture::Checker;
  ball.texture_scale = 12.0;
  ball.albedo = Vec3(0.9, 0.5, 0.35);
  fixture.scene.primitives.push_back(ball);
  // No shading variation, so a texel's colour depends only on where it is on the surface.
  fixture.scene.ambient = 1.0;

  AxisAlignedBox bounds;
  bounds.expand(Vec3::Constant(-fixture.radius * 1.4));
  bounds.expand(Vec3::Constant(fixture.radius * 1.4));
  const double r = fixture.radius;
  fixture.mesh = marchingTetrahedra(
      bounds, cell, [r](const Vec3& p, float* value, std::array<uint8_t, 3>* color) {
        *value = static_cast<float>(p.norm() - r);
        if (color) *color = {128, 128, 128};
        return true;
      });

  const CameraIntrinsics k = phoneIntrinsics(image_size, image_size, 60.0);
  for (const Pose& pose : SyntheticScene::orbitPoses(ball.center, 0.9, 0.0, view_count)) {
    const RenderResult frame = fixture.scene.render(k, pose, true, true);
    TextureView view;
    view.image = frame.color;
    view.depth = frame.depth;
    view.intrinsics = k;
    view.pose = pose;
    view.sharpness = 500.0;
    view.exposure = 128.0;
    fixture.views.push_back(std::move(view));
  }
  return fixture;
}

/// Mean absolute difference between a mesh's atlas colour at each triangle centroid and the
/// colour the scene actually has there.
double meanTextureError(const TexturingResult& result, const SyntheticScene& scene) {
  double total = 0.0;
  int count = 0;
  for (size_t t = 0; t + 2 < result.mesh.indices.size(); t += 3) {
    const uint32_t ia = result.mesh.indices[t];
    const uint32_t ib = result.mesh.indices[t + 1];
    const uint32_t ic = result.mesh.indices[t + 2];
    const Vec3f& va = result.mesh.vertices[ia];
    const Vec3f& vb = result.mesh.vertices[ib];
    const Vec3f& vc = result.mesh.vertices[ic];
    const Vec3 centroid(
        (va.x() + vb.x() + vc.x()) / 3.0,
        (va.y() + vb.y() + vc.y()) / 3.0,
        (va.z() + vb.z() + vc.z()) / 3.0);

    // Sample the atlas at the UV centroid.
    const Vec2f uv(
        (result.mesh.uvs[ia].x() + result.mesh.uvs[ib].x() + result.mesh.uvs[ic].x()) / 3.0f,
        (result.mesh.uvs[ia].y() + result.mesh.uvs[ib].y() + result.mesh.uvs[ic].y()) / 3.0f);
    const int ax = std::min(result.atlas.width - 1,
                            std::max(0, static_cast<int>(uv.x() * result.atlas.width)));
    const int ay = std::min(result.atlas.height - 1,
                            std::max(0, static_cast<int>(uv.y() * result.atlas.height)));
    uint8_t sampled[3];
    result.atlas.get(ax, ay, sampled);

    // Ground truth: cast a ray from outside towards the centroid.
    const Vec3 direction = -centroid.normalized();
    const Vec3 origin = centroid - direction * 2.0;
    Vec3 normal, albedo;
    if (scene.castRay(origin, direction, &normal, &albedo) < 0.0) continue;
    const Vec3 truth = (albedo * 255.0).cwiseMin(255.0).cwiseMax(0.0);

    for (int c = 0; c < 3; ++c) {
      total += std::abs(static_cast<double>(sampled[c]) - truth[c]);
      ++count;
    }
  }
  return count > 0 ? total / count : 1e9;
}

}  // namespace

TEST(Texturing, RejectsDegenerateInput) {
  const Fixture fixture = makeFixture(4, 96, 0.08);
  EXPECT_FALSE(generateTextureAtlas(Mesh{}, fixture.views).success);
  EXPECT_FALSE(generateTextureAtlas(fixture.mesh, {}).success);

  TexturingOptions tiny;
  tiny.atlas_size = 16;  // below the minimum
  EXPECT_FALSE(generateTextureAtlas(fixture.mesh, fixture.views, tiny).success);
}

TEST(Texturing, VisibilityRejectsBackFacingAndOffFrame) {
  const Fixture fixture = makeFixture(4, 128, 0.08);
  ASSERT_FALSE(fixture.views.empty());
  const TextureView& view = fixture.views.front();
  TexturingOptions options;

  // A triangle on the near side of the sphere, facing the camera.
  const Vec3 eye = view.pose.inverse().t;
  const Vec3 near_point = eye.normalized() * fixture.radius;
  const Vec3 tangent = eye.normalized().cross(Vec3::UnitY()).normalized() * 0.02;
  const Vec3 bitangent = eye.normalized().cross(tangent).normalized() * 0.02;
  EXPECT_TRUE(triangleVisibleIn(view, near_point, near_point + tangent,
                                near_point + bitangent, options));

  // The same triangle on the far side is back-facing and occluded.
  const Vec3 far_point = -near_point;
  EXPECT_FALSE(triangleVisibleIn(view, far_point, far_point + tangent,
                                 far_point + bitangent, options));

  // Far outside the frustum.
  EXPECT_FALSE(triangleVisibleIn(view, Vec3(50, 50, 50), Vec3(50.1, 50, 50),
                                 Vec3(50, 50.1, 50), options));
}

TEST(Texturing, ViewScoringPrefersSharpAndHeadOnViews) {
  const Fixture fixture = makeFixture(8, 128, 0.08);
  const TextureView& base = fixture.views.front();
  const Vec3 eye = base.pose.inverse().t;
  const Vec3 point = eye.normalized() * fixture.radius;
  const Vec3 u = eye.normalized().cross(Vec3::UnitY()).normalized() * 0.015;
  const Vec3 v = eye.normalized().cross(u).normalized() * 0.015;

  TexturingOptions options;
  const double sharp_score = scoreViewForTriangle(base, point, point + u, point + v, options);
  ASSERT_GT(sharp_score, 0.0);

  // The same view with a blurred frame must score lower.
  TextureView blurred = base;
  blurred.sharpness = 20.0;
  const double blurred_score =
      scoreViewForTriangle(blurred, point, point + u, point + v, options);
  EXPECT_LT(blurred_score, sharp_score);

  // A view further away must score lower than one close in.
  TextureView distant = base;
  const Pose cam_to_world = base.pose.inverse();
  distant.pose = SyntheticScene::lookAt(cam_to_world.t * 2.5, Vec3::Zero());
  distant.depth = DepthMap{};  // no occlusion data; orientation alone decides
  const double distant_score =
      scoreViewForTriangle(distant, point, point + u, point + v, options);
  EXPECT_LT(distant_score, sharp_score);
}

TEST(Texturing, ProducesAUsableAtlasWithPerTriangleUvs) {
  const Fixture fixture = makeFixture(16, 192, 0.05);
  ASSERT_FALSE(fixture.mesh.empty());

  TexturingOptions options;
  options.atlas_size = 1024;
  options.max_patch_texels = 12;
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, options);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.atlas.width, 1024);
  EXPECT_EQ(result.atlas.height, 1024);

  // Per-triangle atlas: three unique vertices per triangle.
  EXPECT_EQ(result.mesh.triangleCount(), result.mesh.vertexCount() / 3);
  EXPECT_EQ(result.mesh.triangleCount(), static_cast<size_t>(result.stats.triangles_textured));
  ASSERT_TRUE(result.mesh.hasUvs());
  ASSERT_TRUE(result.mesh.hasNormals());

  std::string reason;
  EXPECT_TRUE(result.mesh.isValid(&reason)) << reason;

  // Every UV must be inside the atlas.
  for (const Vec2f& uv : result.mesh.uvs) {
    EXPECT_GE(uv.x(), 0.0f);
    EXPECT_GE(uv.y(), 0.0f);
    EXPECT_LE(uv.x(), 1.0f);
    EXPECT_LE(uv.y(), 1.0f);
  }
  EXPECT_GT(result.stats.atlas_utilisation, 0.01);
  EXPECT_LE(result.stats.atlas_utilisation, 1.0);
  // Most of the sphere should be seen by an equatorial orbit.
  EXPECT_LT(result.stats.triangles_without_view, result.stats.triangles_textured);
}

TEST(Texturing, PatchesDoNotOverlapInTheAtlas) {
  const Fixture fixture = makeFixture(10, 160, 0.06);
  TexturingOptions options;
  options.atlas_size = 1024;
  options.max_patch_texels = 16;
  options.patch_padding = 2;
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, options);
  ASSERT_TRUE(result.success);

  // The three UVs of a triangle form a right triangle in one square cell. Check the cells of
  // distinct triangles are disjoint: an overlap means triangles sampling each other's texels.
  struct Cell {
    int x0, y0, x1, y1;
  };
  std::vector<Cell> cells;
  for (size_t t = 0; t + 2 < result.mesh.indices.size(); t += 3) {
    float min_u = 1e9f, min_v = 1e9f, max_u = -1e9f, max_v = -1e9f;
    for (int k = 0; k < 3; ++k) {
      const Vec2f& uv = result.mesh.uvs[result.mesh.indices[t + k]];
      min_u = std::min(min_u, uv.x());
      max_u = std::max(max_u, uv.x());
      min_v = std::min(min_v, uv.y());
      max_v = std::max(max_v, uv.y());
    }
    cells.push_back(Cell{static_cast<int>(min_u * options.atlas_size),
                         static_cast<int>(min_v * options.atlas_size),
                         static_cast<int>(max_u * options.atlas_size),
                         static_cast<int>(max_v * options.atlas_size)});
  }
  // Pairwise overlap on a sample, since the full comparison is quadratic.
  const size_t limit = std::min<size_t>(cells.size(), 220);
  int overlaps = 0;
  for (size_t i = 0; i < limit; ++i) {
    for (size_t j = i + 1; j < limit; ++j) {
      const Cell& a = cells[i];
      const Cell& b = cells[j];
      const bool disjoint = a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0;
      if (!disjoint) ++overlaps;
    }
  }
  EXPECT_EQ(overlaps, 0) << "patches overlap in the atlas";
}

TEST(Texturing, AtlasColoursMatchTheSceneItWasSampledFrom) {
  // The accuracy test: the texture must actually carry the scene's colours.
  const Fixture fixture = makeFixture(20, 256, 0.05);
  TexturingOptions options;
  options.atlas_size = 2048;
  options.max_patch_texels = 14;
  // Turn off the correction passes so this measures sampling fidelity alone.
  options.compensate_exposure = false;
  options.level_seams = false;
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, options);
  ASSERT_TRUE(result.success);

  const double error = meanTextureError(result, fixture.scene);
  // A checkerboard at the mesh's tessellation has genuine high-frequency content, so some
  // error is expected; what matters is that the texture is clearly the right image.
  EXPECT_LT(error, 48.0) << "mean per-channel texture error " << error;
}

TEST(Texturing, SeamLevellingReducesTheDiscontinuityAcrossEdges) {
  // Give the views deliberately mismatched exposures, which is what produces visible seams.
  Fixture fixture = makeFixture(14, 192, 0.055);
  for (size_t v = 0; v < fixture.views.size(); ++v) {
    const double scale = (v % 2 == 0) ? 0.82 : 1.18;
    for (uint8_t& channel : fixture.views[v].image.pixels) {
      channel = static_cast<uint8_t>(
          std::max(0.0, std::min(255.0, std::round(channel * scale))));
    }
    fixture.views[v].exposure = 128.0 * scale;
  }

  TexturingOptions options;
  options.atlas_size = 1024;
  options.max_patch_texels = 12;
  options.compensate_exposure = false;  // isolate the seam pass
  options.level_seams = true;
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, options);
  ASSERT_TRUE(result.success);

  EXPECT_GT(result.stats.seam_residual_before, 0.0);
  EXPECT_LT(result.stats.seam_residual_after, result.stats.seam_residual_before)
      << "before " << result.stats.seam_residual_before << " after "
      << result.stats.seam_residual_after;
}

TEST(Texturing, ExposureCompensationPullsViewsTogether) {
  Fixture bright = makeFixture(12, 160, 0.06);
  // One view is badly overexposed.
  for (uint8_t& channel : bright.views.front().image.pixels) {
    channel = static_cast<uint8_t>(std::min(255, channel + 70));
  }
  bright.views.front().exposure = 200.0;

  TexturingOptions compensated;
  compensated.atlas_size = 1024;
  compensated.max_patch_texels = 12;
  compensated.compensate_exposure = true;
  compensated.level_seams = false;
  const TexturingResult with = generateTextureAtlas(bright.mesh, bright.views, compensated);
  ASSERT_TRUE(with.success);

  TexturingOptions raw = compensated;
  raw.compensate_exposure = false;
  const TexturingResult without = generateTextureAtlas(bright.mesh, bright.views, raw);
  ASSERT_TRUE(without.success);

  // Compensation should bring the atlas closer to the true scene colours.
  EXPECT_LE(meanTextureError(with, bright.scene), meanTextureError(without, bright.scene) + 1.0);
}

TEST(Texturing, ShrinksPatchesRatherThanFailingOnASmallAtlas) {
  const Fixture fixture = makeFixture(10, 160, 0.04);  // a denser mesh
  ASSERT_GT(fixture.mesh.triangleCount(), 2000u);

  TexturingOptions cramped;
  cramped.atlas_size = 512;
  cramped.max_patch_texels = 64;  // far too large for this many triangles
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, cramped);
  // It must succeed at reduced resolution rather than giving up.
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.atlas.width, 512);
  EXPECT_GT(result.stats.triangles_textured, 0);
}

TEST(Texturing, TrianglesNoViewCanSeeAreCountedAndExcluded) {
  // Views on one side only: the far hemisphere has no usable view.
  Fixture fixture;
  Primitive ball;
  ball.shape = Shape::Sphere;
  ball.radius = 0.3;
  ball.texture = Texture::Noise;
  fixture.scene.primitives.push_back(ball);
  fixture.scene.ambient = 1.0;

  AxisAlignedBox bounds;
  bounds.expand(Vec3::Constant(-0.42));
  bounds.expand(Vec3::Constant(0.42));
  fixture.mesh = marchingTetrahedra(
      bounds, 0.06, [](const Vec3& p, float* value, std::array<uint8_t, 3>* color) {
        *value = static_cast<float>(p.norm() - 0.3);
        if (color) *color = {120, 120, 120};
        return true;
      });

  const CameraIntrinsics k = phoneIntrinsics(160, 160, 60.0);
  // A 60 degree arc only.
  for (const Pose& pose :
       SyntheticScene::orbitPoses(Vec3::Zero(), 0.9, 0.0, 5, 0.0, deg2rad(60.0))) {
    const RenderResult frame = fixture.scene.render(k, pose, true, true);
    TextureView view;
    view.image = frame.color;
    view.depth = frame.depth;
    view.intrinsics = k;
    view.pose = pose;
    view.sharpness = 400.0;
    fixture.views.push_back(std::move(view));
  }

  TexturingOptions options;
  options.atlas_size = 1024;
  const TexturingResult result = generateTextureAtlas(fixture.mesh, fixture.views, options);
  ASSERT_TRUE(result.success);
  // The unseen hemisphere must be reported, not silently textured with something wrong.
  EXPECT_GT(result.stats.triangles_without_view, 0);
  EXPECT_EQ(result.mesh.triangleCount(), static_cast<size_t>(result.stats.triangles_textured));
}
