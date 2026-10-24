#include "parallax/mesh/Mesh.h"

#include <gtest/gtest.h>

#include "parallax/fusion/TsdfVolume.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// A closed unit cube: 8 vertices, 12 triangles, outward winding.
Mesh unitCube() {
  Mesh mesh;
  mesh.vertices = {
      Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0),
      Vec3f(0, 0, 1), Vec3f(1, 0, 1), Vec3f(1, 1, 1), Vec3f(0, 1, 1),
  };
  mesh.indices = {
      0, 2, 1, 0, 3, 2,  // z = 0 (normal -z)
      4, 5, 6, 4, 6, 7,  // z = 1 (normal +z)
      0, 1, 5, 0, 5, 4,  // y = 0
      2, 3, 7, 2, 7, 6,  // y = 1
      0, 4, 7, 0, 7, 3,  // x = 0
      1, 2, 6, 1, 6, 5,  // x = 1
  };
  recomputeNormals(&mesh);
  return mesh;
}

/// A flat grid in the xy plane with `n` x `n` quads; has a boundary.
Mesh planeGrid(int n) {
  Mesh mesh;
  for (int y = 0; y <= n; ++y) {
    for (int x = 0; x <= n; ++x) {
      mesh.vertices.push_back(Vec3f(static_cast<float>(x) / n, static_cast<float>(y) / n, 0.f));
    }
  }
  const auto id = [n](int x, int y) { return static_cast<uint32_t>(y * (n + 1) + x); };
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      mesh.indices.insert(mesh.indices.end(),
                          {id(x, y), id(x + 1, y), id(x + 1, y + 1)});
      mesh.indices.insert(mesh.indices.end(),
                          {id(x, y), id(x + 1, y + 1), id(x, y + 1)});
    }
  }
  recomputeNormals(&mesh);
  return mesh;
}

/// Extract a sphere mesh via the isosurface extractor.
Mesh sphereMesh(double radius, double cell) {
  AxisAlignedBox bounds;
  bounds.expand(Vec3::Constant(-radius * 1.4));
  bounds.expand(Vec3::Constant(radius * 1.4));
  return marchingTetrahedra(bounds, cell,
                            [radius](const Vec3& p, float* v, std::array<uint8_t, 3>* c) {
                              *v = static_cast<float>(p.norm() - radius);
                              if (c) *c = {120, 160, 200};
                              return true;
                            });
}

}  // namespace

// ------------------------------------------------------------- validation ---

TEST(Mesh, ValidatesStructure) {
  Mesh mesh = unitCube();
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;

  Mesh bad_index = mesh;
  bad_index.indices[0] = 999;
  EXPECT_FALSE(bad_index.isValid(&reason));
  EXPECT_NE(reason.find("out of range"), std::string::npos);

  Mesh bad_count = mesh;
  bad_count.indices.pop_back();
  EXPECT_FALSE(bad_count.isValid(&reason));

  Mesh bad_normals = mesh;
  bad_normals.normals.pop_back();
  EXPECT_FALSE(bad_normals.isValid(&reason));

  Mesh non_finite = mesh;
  non_finite.vertices[0] = Vec3f(std::nanf(""), 0.f, 0.f);
  EXPECT_FALSE(non_finite.isValid(&reason));
}

TEST(Mesh, BoundsAreaAndVolumeOfAUnitCube) {
  const Mesh cube = unitCube();
  const AxisAlignedBox box = meshBounds(cube);
  EXPECT_TRUE(box.valid());
  EXPECT_LT((box.min - Vec3::Zero()).norm(), 1e-6);
  EXPECT_LT((box.max - Vec3::Ones()).norm(), 1e-6);
  EXPECT_NEAR(box.diagonal(), std::sqrt(3.0), 1e-6);

  EXPECT_NEAR(meshSurfaceArea(cube), 6.0, 1e-9);
  EXPECT_NEAR(meshSignedVolume(cube), 1.0, 1e-9);
}

TEST(Mesh, SignedVolumeIsNegativeForInvertedWinding) {
  Mesh cube = unitCube();
  flipOrientation(&cube);
  EXPECT_NEAR(meshSignedVolume(cube), -1.0, 1e-9);
  // Area is orientation independent.
  EXPECT_NEAR(meshSurfaceArea(cube), 6.0, 1e-9);
}

TEST(Mesh, NormalsAreUnitLengthAndOutward) {
  const Mesh cube = unitCube();
  ASSERT_TRUE(cube.hasNormals());
  const Vec3 centre(0.5, 0.5, 0.5);
  for (size_t i = 0; i < cube.vertices.size(); ++i) {
    const Vec3 n(cube.normals[i].x(), cube.normals[i].y(), cube.normals[i].z());
    EXPECT_NEAR(n.norm(), 1.0, 1e-6);
    const Vec3 p(cube.vertices[i].x(), cube.vertices[i].y(), cube.vertices[i].z());
    EXPECT_GT(n.dot((p - centre).normalized()), 0.0);
  }
}

TEST(Mesh, TopologyOfAClosedCubeAndAnOpenPlane) {
  const Mesh cube = unitCube();
  EXPECT_TRUE(isWatertight(cube));
  EXPECT_EQ(eulerCharacteristic(cube), 2);  // genus 0
  EXPECT_TRUE(findBoundaryLoops(cube).empty());

  const Mesh plane = planeGrid(4);
  EXPECT_FALSE(isWatertight(plane));
  const std::vector<BoundaryLoop> loops = findBoundaryLoops(plane);
  ASSERT_EQ(loops.size(), 1u);
  // The boundary of a 4x4 grid is 16 vertices with a perimeter of 4 unit edges.
  EXPECT_EQ(loops[0].vertices.size(), 16u);
  EXPECT_NEAR(loops[0].length, 4.0, 1e-6);
}

TEST(Mesh, EmptyMeshIsHandledEverywhere) {
  Mesh empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(isWatertight(empty));
  EXPECT_EQ(meshSurfaceArea(empty), 0.0);
  EXPECT_EQ(meshSignedVolume(empty), 0.0);
  EXPECT_FALSE(meshBounds(empty).valid());
  EXPECT_TRUE(findBoundaryLoops(empty).empty());
  EXPECT_EQ(fillHoles(&empty), 0);
  EXPECT_EQ(decimateMesh(&empty, 10), 0);
  EXPECT_EQ(removeSmallComponents(&empty), 0);
  EXPECT_EQ(keepLargestComponent(&empty), 0);
  smoothMesh(&empty, 2);  // must not crash
  recomputeNormals(&empty);
  EXPECT_TRUE(generateLods(empty).empty());

  // Null pointers must be tolerated too.
  EXPECT_EQ(fillHoles(nullptr), 0);
  EXPECT_EQ(decimateMesh(nullptr, 10), 0);
  EXPECT_EQ(weldVertices(nullptr), 0);
  EXPECT_EQ(removeUnreferencedVertices(nullptr), 0);
  recomputeNormals(nullptr);
  smoothMesh(nullptr, 1);
}

// --------------------------------------------------------------- welding ---

TEST(Mesh, WeldMergesDuplicateVertices) {
  // Two triangles sharing an edge, but with the shared vertices duplicated.
  Mesh mesh;
  mesh.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0),
                   Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0)};
  mesh.indices = {0, 1, 2, 3, 5, 4};
  std::vector<int> before_labels;
  EXPECT_EQ(labelConnectedComponents(mesh, &before_labels), 2);

  const int removed = weldVertices(&mesh, 1e-9);
  EXPECT_EQ(removed, 2);
  EXPECT_EQ(mesh.vertexCount(), 4u);
  EXPECT_EQ(mesh.triangleCount(), 2u);
  // Now they share an edge, so they form one component.
  std::vector<int> labels;
  EXPECT_EQ(labelConnectedComponents(mesh, &labels), 1);
}

TEST(Mesh, WeldFindsNeighboursAcrossCellBoundaries) {
  // Two vertices within tolerance but straddling a hash cell boundary: a naive
  // single-cell lookup would miss them.
  Mesh mesh;
  const double tolerance = 1e-4;
  mesh.vertices = {Vec3f(0.f, 0.f, 0.f),
                   Vec3f(static_cast<float>(tolerance * 0.4), 0.f, 0.f),
                   Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f)};
  mesh.indices = {0, 2, 3, 1, 3, 2};
  const int removed = weldVertices(&mesh, tolerance);
  EXPECT_EQ(removed, 1);
  EXPECT_EQ(mesh.vertexCount(), 3u);
}

TEST(Mesh, WeldPreservesAttributes) {
  Mesh mesh;
  mesh.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 0, 0)};
  mesh.colors = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {4, 5, 6}};
  mesh.uvs = {Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1), Vec2f(1, 0)};
  mesh.indices = {0, 1, 2, 0, 3, 2};
  weldVertices(&mesh, 1e-9);
  EXPECT_EQ(mesh.colors.size(), mesh.vertices.size());
  EXPECT_EQ(mesh.uvs.size(), mesh.vertices.size());
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;
}

TEST(Mesh, RemoveDegenerateDropsZeroAreaAndRepeatedIndices) {
  Mesh mesh;
  mesh.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(2, 0, 0)};
  mesh.indices = {
      0, 1, 2,  // good
      0, 1, 1,  // repeated index
      0, 1, 3,  // collinear, zero area
  };
  const int removed = removeDegenerateTriangles(&mesh);
  EXPECT_EQ(removed, 2);
  EXPECT_EQ(mesh.triangleCount(), 1u);
}

TEST(Mesh, RemoveUnreferencedCompactsAndRemaps) {
  Mesh mesh = unitCube();
  mesh.vertices.push_back(Vec3f(9, 9, 9));  // orphan
  mesh.normals.push_back(Vec3f(0, 1, 0));
  const int removed = removeUnreferencedVertices(&mesh);
  EXPECT_EQ(removed, 1);
  EXPECT_EQ(mesh.vertexCount(), 8u);
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;
  EXPECT_NEAR(meshSignedVolume(mesh), 1.0, 1e-9);
}

// ------------------------------------------------------------ components ---

TEST(Mesh, ComponentsAreLabelledAndSmallOnesRemoved) {
  Mesh mesh = sphereMesh(0.5, 0.05);
  ASSERT_FALSE(mesh.empty());
  const size_t sphere_triangles = mesh.triangleCount();

  // Add a tiny floating fragment, as depth noise produces.
  Mesh debris;
  debris.vertices = {Vec3f(5, 5, 5), Vec3f(5.01f, 5, 5), Vec3f(5, 5.01f, 5)};
  debris.indices = {0, 1, 2};
  recomputeNormals(&debris);
  mesh.append(debris);

  std::vector<int> labels;
  EXPECT_EQ(labelConnectedComponents(mesh, &labels), 2);

  const int removed = removeSmallComponents(&mesh, 64, 0.02);
  EXPECT_EQ(removed, 1);
  EXPECT_EQ(mesh.triangleCount(), sphere_triangles);
  EXPECT_EQ(labelConnectedComponents(mesh, &labels), 1);
}

TEST(Mesh, KeepLargestComponentDiscardsTheRest) {
  Mesh mesh = sphereMesh(0.5, 0.06);
  const size_t big = mesh.triangleCount();
  Mesh small = sphereMesh(0.1, 0.03);
  transformMesh(&small, Mat3::Identity(), Vec3(4, 0, 0));
  mesh.append(small);

  std::vector<int> labels;
  ASSERT_EQ(labelConnectedComponents(mesh, &labels), 2);
  keepLargestComponent(&mesh);
  EXPECT_EQ(labelConnectedComponents(mesh, &labels), 1);
  EXPECT_EQ(mesh.triangleCount(), big);
  // The surviving component must be the one at the origin.
  EXPECT_LT(meshBounds(mesh).center().norm(), 0.1);
}

TEST(Mesh, AppendOffsetsIndicesAndPadsAttributes) {
  Mesh a = unitCube();  // has normals, no colors
  Mesh b = unitCube();
  b.colors.assign(b.vertexCount(), {10, 20, 30});
  transformMesh(&b, Mat3::Identity(), Vec3(3, 0, 0));

  a.append(b);
  std::string reason;
  EXPECT_TRUE(a.isValid(&reason)) << reason;
  EXPECT_EQ(a.vertexCount(), 16u);
  EXPECT_EQ(a.triangleCount(), 24u);
  // Colours existed on only one side, so the result must be padded to full
  // length rather than left half-sized.
  EXPECT_EQ(a.colors.size(), a.vertexCount());
  EXPECT_EQ(a.normals.size(), a.vertexCount());
  EXPECT_NEAR(meshSurfaceArea(a), 12.0, 1e-9);
}

// ----------------------------------------------------------- hole filling ---

TEST(Mesh, FillHolesClosesAPuncturedSphere) {
  Mesh mesh = sphereMesh(0.5, 0.04);
  ASSERT_TRUE(isWatertight(mesh));
  const double volume_before = meshSignedVolume(mesh);

  // Punch a hole by deleting the triangles near the north pole.
  std::vector<uint32_t> kept;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec3f& v = mesh.vertices[mesh.indices[t]];
    if (v.z() > 0.42f) continue;
    kept.push_back(mesh.indices[t]);
    kept.push_back(mesh.indices[t + 1]);
    kept.push_back(mesh.indices[t + 2]);
  }
  mesh.indices = std::move(kept);
  removeUnreferencedVertices(&mesh);
  ASSERT_FALSE(isWatertight(mesh));
  ASSERT_FALSE(findBoundaryLoops(mesh).empty());

  const int filled = fillHoles(&mesh, 500);
  EXPECT_GE(filled, 1);
  EXPECT_TRUE(isWatertight(mesh)) << "hole filling should close the surface";
  EXPECT_TRUE(findBoundaryLoops(mesh).empty());
  // The patch must preserve orientation, so the volume stays positive and close
  // to the original.
  const double volume_after = meshSignedVolume(mesh);
  EXPECT_GT(volume_after, 0.0);
  EXPECT_NEAR(volume_after, volume_before, volume_before * 0.1);
}

TEST(Mesh, FillHolesLeavesLargeOpeningsAlone) {
  // Deliberately capped: a big opening is usually the genuinely unseen side of
  // an object, and inventing a surface there would be a lie.
  Mesh mesh = planeGrid(8);
  const std::vector<BoundaryLoop> loops = findBoundaryLoops(mesh);
  ASSERT_EQ(loops.size(), 1u);
  const int filled = fillHoles(&mesh, static_cast<int>(loops[0].vertices.size()) - 1);
  EXPECT_EQ(filled, 0);
  EXPECT_FALSE(isWatertight(mesh));
}

TEST(Mesh, FillHolesClosesAFlatGridIntoADisc) {
  Mesh mesh = planeGrid(4);
  const int filled = fillHoles(&mesh, 500);
  EXPECT_EQ(filled, 1);
  EXPECT_TRUE(isWatertight(mesh));
}

// ------------------------------------------------------------- decimation ---

TEST(Mesh, DecimationReachesTheTargetAndPreservesShape) {
  Mesh mesh = sphereMesh(0.5, 0.025);
  ASSERT_GT(mesh.triangleCount(), 4000u);
  const double area_before = meshSurfaceArea(mesh);
  const double volume_before = meshSignedVolume(mesh);
  const AxisAlignedBox bounds_before = meshBounds(mesh);

  const int target = static_cast<int>(mesh.triangleCount() / 4);
  const int achieved = decimateMesh(&mesh, target, /*preserve_boundary=*/false);

  EXPECT_LE(achieved, static_cast<int>(target * 1.15)) << "should get close to the target";
  EXPECT_EQ(achieved, static_cast<int>(mesh.triangleCount()));
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;

  // Shape must survive: a quadric metric preserves area and volume well.
  EXPECT_NEAR(meshSurfaceArea(mesh), area_before, area_before * 0.06);
  EXPECT_NEAR(meshSignedVolume(mesh), volume_before, volume_before * 0.06);
  const AxisAlignedBox bounds_after = meshBounds(mesh);
  EXPECT_LT((bounds_after.min - bounds_before.min).norm(), 0.03);
  EXPECT_LT((bounds_after.max - bounds_before.max).norm(), 0.03);
}

TEST(Mesh, DecimationKeepsTheSurfaceClosed) {
  Mesh mesh = sphereMesh(0.5, 0.03);
  ASSERT_TRUE(isWatertight(mesh));
  decimateMesh(&mesh, static_cast<int>(mesh.triangleCount() / 3), false);
  // The link condition and the normal-flip test exist precisely so decimation
  // cannot tear the surface open or fold it over.
  EXPECT_TRUE(isWatertight(mesh));
  EXPECT_EQ(eulerCharacteristic(mesh), 2);
  EXPECT_GT(meshSignedVolume(mesh), 0.0);
}

TEST(Mesh, DecimationPreservesBoundaryWhenAsked) {
  Mesh mesh = planeGrid(12);
  const std::vector<BoundaryLoop> before = findBoundaryLoops(mesh);
  ASSERT_EQ(before.size(), 1u);

  decimateMesh(&mesh, 40, /*preserve_boundary=*/true);
  const std::vector<BoundaryLoop> after = findBoundaryLoops(mesh);
  ASSERT_EQ(after.size(), 1u);
  // The perimeter must be untouched.
  EXPECT_EQ(after[0].vertices.size(), before[0].vertices.size());
  EXPECT_NEAR(after[0].length, before[0].length, 1e-6);
}

TEST(Mesh, DecimationIsANoOpBelowTheTarget) {
  Mesh mesh = unitCube();
  const size_t before = mesh.triangleCount();
  EXPECT_EQ(decimateMesh(&mesh, 1000), static_cast<int>(before));
  EXPECT_EQ(mesh.triangleCount(), before);
}

TEST(Mesh, DecimationToZeroClearsTheMesh) {
  Mesh mesh = unitCube();
  EXPECT_EQ(decimateMesh(&mesh, 0), 0);
  EXPECT_TRUE(mesh.empty());
}

TEST(Mesh, LodChainShrinksMonotonically) {
  const Mesh mesh = sphereMesh(0.5, 0.03);
  const std::vector<Mesh> lods = generateLods(mesh, 3, 0.5);
  ASSERT_GE(lods.size(), 2u);
  size_t previous = mesh.triangleCount();
  for (const Mesh& lod : lods) {
    EXPECT_LT(lod.triangleCount(), previous);
    std::string reason;
    EXPECT_TRUE(lod.isValid(&reason)) << reason;
    previous = lod.triangleCount();
  }
}

// -------------------------------------------------------------- smoothing ---

TEST(Mesh, SmoothingReducesNoise) {
  Mesh mesh = sphereMesh(0.5, 0.035);
  const auto radialSpread = [](const Mesh& m) {
    double sum = 0.0, sum_sq = 0.0;
    for (const Vec3f& v : m.vertices) {
      const double r = Vec3(v.x(), v.y(), v.z()).norm();
      sum += r;
      sum_sq += r * r;
    }
    const double n = static_cast<double>(m.vertices.size());
    return std::sqrt(std::max(0.0, sum_sq / n - (sum / n) * (sum / n)));
  };

  Rng rng(3);
  for (Vec3f& v : mesh.vertices) {
    v += Vec3f(static_cast<float>(rng.normal()), static_cast<float>(rng.normal()),
               static_cast<float>(rng.normal())) *
         0.01f;
  }
  const double before = radialSpread(mesh);
  smoothMesh(&mesh, 4, 0.5, false);
  EXPECT_LT(radialSpread(mesh), before);
}

TEST(Mesh, SmoothingHoldsTheBoundaryStill) {
  Mesh mesh = planeGrid(6);
  const std::vector<BoundaryLoop> loops = findBoundaryLoops(mesh);
  ASSERT_EQ(loops.size(), 1u);
  std::vector<Vec3f> boundary_before;
  for (uint32_t v : loops[0].vertices) boundary_before.push_back(mesh.vertices[v]);

  // Displace the interior, then smooth with boundary preservation on.
  Rng rng(8);
  for (Vec3f& v : mesh.vertices) v.z() += static_cast<float>(rng.normal()) * 0.05f;
  smoothMesh(&mesh, 5, 0.6, /*preserve_boundary=*/true);

  for (size_t i = 0; i < loops[0].vertices.size(); ++i) {
    const Vec3f& now = mesh.vertices[loops[0].vertices[i]];
    // x and y must be exactly as before; z was perturbed before smoothing but
    // must then have been held fixed.
    EXPECT_FLOAT_EQ(now.x(), boundary_before[i].x());
    EXPECT_FLOAT_EQ(now.y(), boundary_before[i].y());
  }
}

// ----------------------------------------------------------- transform/crop ---

TEST(Mesh, TransformAppliesRotationTranslationAndScale) {
  Mesh mesh = unitCube();
  const Mat3 R = so3Exp(Vec3(0, kPi / 2, 0));
  transformMesh(&mesh, R, Vec3(1, 2, 3), 2.0);

  // Volume scales with the cube of the linear scale factor.
  EXPECT_NEAR(meshSignedVolume(mesh), 8.0, 1e-6);
  EXPECT_NEAR(meshSurfaceArea(mesh), 24.0, 1e-6);
  const AxisAlignedBox box = meshBounds(mesh);
  EXPECT_NEAR(box.extent().x(), 2.0, 1e-6);
  // Normals must stay unit length under a uniform scale.
  for (const Vec3f& n : mesh.normals) EXPECT_NEAR(n.norm(), 1.0f, 1e-5f);
}

TEST(Mesh, CropKeepsOnlyTheRequestedRegion) {
  Mesh mesh = sphereMesh(0.5, 0.04);
  AxisAlignedBox box;
  box.expand(Vec3(-1, -1, 0));
  box.expand(Vec3(1, 1, 1));

  const int removed = cropMesh(&mesh, box);
  EXPECT_GT(removed, 0);
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec3f& a = mesh.vertices[mesh.indices[t]];
    const Vec3f& b = mesh.vertices[mesh.indices[t + 1]];
    const Vec3f& c = mesh.vertices[mesh.indices[t + 2]];
    const double centroid_z = (a.z() + b.z() + c.z()) / 3.0;
    EXPECT_GE(centroid_z, -1e-6);
  }
  std::string reason;
  EXPECT_TRUE(mesh.isValid(&reason)) << reason;
}

TEST(Mesh, CropWithAnInvalidBoxIsANoOp) {
  Mesh mesh = unitCube();
  const size_t before = mesh.triangleCount();
  EXPECT_EQ(cropMesh(&mesh, AxisAlignedBox{}), 0);
  EXPECT_EQ(mesh.triangleCount(), before);
}
