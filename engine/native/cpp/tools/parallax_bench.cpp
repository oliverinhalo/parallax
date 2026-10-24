// Microbenchmarks for the engine's hot paths.
//
// A separate binary rather than a gtest: these are measurements, not
// assertions, and a test that fails because a machine was busy is worse than
// no test. CI runs it and records the numbers, so a change that halves the
// speed of feature detection shows up in the log of the run that caused it.
//
// Each case reports the median of its runs, not the mean. One slow run from a
// scheduler hiccup moves a mean and not a median, and the median is what a
// user's experience is closer to anyway.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "parallax/depth/MultiViewStereo.h"
#include "parallax/exporters/HtmlViewer.h"
#include "parallax/features/Features.h"
#include "parallax/fusion/TsdfVolume.h"
#include "parallax/io/ImageIo.h"
#include "parallax/mesh/Mesh.h"
#include "parallax/splat/SplatPreview.h"
#include "parallax/splat/SplatRaster.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;
using Clock = std::chrono::steady_clock;

namespace {

struct Case {
  std::string name;
  /// What one unit of work is, so the number has a unit: "frame", "megapixel".
  std::string unit;
  double units_per_run = 1.0;
  std::function<void()> run;
};

double medianMilliseconds(const std::function<void()>& body, int runs) {
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(runs));
  for (int i = 0; i < runs; ++i) {
    const auto start = Clock::now();
    body();
    const auto end = Clock::now();
    samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

/// A textured scene and one rendered view of it, built once and reused.
struct Fixtures {
  SyntheticScene scene;
  CameraIntrinsics intrinsics;
  RenderResult frame;
  std::vector<uint8_t> png;
  GaussianCloud cloud;
  SplatView splat_view;
  std::vector<float> target;
  Mesh mesh;

  Fixtures() {
    Primitive ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0, 0.3, 0.0);
    ball.radius = 0.25;
    ball.texture = Texture::Noise;
    ball.texture_scale = 30.0;
    scene.primitives.push_back(ball);
    Primitive floor;
    floor.shape = Shape::Plane;
    floor.normal = Vec3::UnitY();
    floor.texture = Texture::Checker;
    floor.texture_scale = 6.0;
    scene.primitives.push_back(floor);

    intrinsics = phoneIntrinsics(640, 480, 62.0);
    const Pose pose = SyntheticScene::orbitPoses(ball.center, 0.9, 0.25, 1).front();
    frame = scene.render(intrinsics, pose, true, true);
    encodePng(frame.color, &png, 4);

    // A small splat field over a 96x96 view: enough to exercise every term in
    // the backward pass without making the benchmark take a minute.
    std::vector<Vec3f> points;
    std::vector<std::array<uint8_t, 3>> colors;
    Rng rng(7);
    for (int i = 0; i < 4000; ++i) {
      points.emplace_back(static_cast<float>(rng.uniform(-0.3, 0.3)),
                          static_cast<float>(rng.uniform(-0.3, 0.3)),
                          static_cast<float>(rng.uniform(-0.3, 0.3)));
      colors.push_back({static_cast<uint8_t>(i % 256), 120, 200});
    }
    cloud = initializeFromPointCloud(points, colors, 0);
    splat_view.intrinsics = phoneIntrinsics(96, 96, 55.0);
    splat_view.camera_from_world = Pose(Mat3::Identity(), Vec3(0.0, 0.0, 1.4));
    target.assign(96ull * 96 * 3, 0.5f);

    mesh = buildPointCloudPreview(points, {}, colors);
  }
};

}  // namespace

int main(int argc, char** argv) {
  int runs = 7;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) runs = std::atoi(argv[++i]);
  }
  runs = std::max(runs, 1);

  Fixtures f;
  const double megapixels =
      static_cast<double>(f.frame.color.width) * f.frame.color.height / 1e6;

  std::vector<Case> cases;

  const GrayBuffer gray = toGray(f.frame.color);
  cases.push_back({"features.detectAndDescribe", "megapixel", megapixels, [&] {
    detectAndDescribe(gray.view(), {});
  }});

  // Everything a case closes over has to outlive the case list, which is why
  // these are declared here rather than in a block beside the case that uses
  // them: a `[&]` lambda over a block-scoped fixture is a dangling reference by
  // the time the timing loop runs.
  const FeatureSet features = detectAndDescribe(gray.view(), {});
  cases.push_back({
      "features.match", "keypoint", static_cast<double>(features.keypoints.size()),
      [&] { matchFeatures(features, features, {}); },
  });

  cases.push_back({"png.decode", "megapixel", megapixels, [&] {
    ColorBuffer out;
    decodePng(f.png, &out);
  }});

  cases.push_back({"png.encode", "megapixel", megapixels, [&] {
    std::vector<uint8_t> png;
    encodePng(f.frame.color, &png, 4);
  }});

  // MVS at the resolution the pipeline actually runs it: the pixel loop is
  // the single most expensive thing in a reconstruction without a depth sensor.
  const CameraIntrinsics mvs_intrinsics = phoneIntrinsics(240, 180, 62.0);
  const std::vector<Pose> mvs_poses =
      SyntheticScene::orbitPoses(Vec3(0.0, 0.3, 0.0), 0.9, 0.25, 3);
  std::vector<GrayBuffer> mvs_grays;
  for (const Pose& pose : mvs_poses) {
    mvs_grays.push_back(toGray(f.scene.render(mvs_intrinsics, pose, true, false).color));
  }
  std::vector<MvsView> mvs_views;
  for (size_t i = 0; i < mvs_poses.size(); ++i) {
    MvsView view;
    view.gray = &mvs_grays[i];
    view.intrinsics = mvs_intrinsics;
    view.pose = mvs_poses[i];
    mvs_views.push_back(view);
  }
  const std::vector<MvsView> neighbours(mvs_views.begin() + 1, mvs_views.end());
  cases.push_back({"mvs.estimateDepth", "megapixel", 240.0 * 180.0 / 1e6, [&] {
    MvsConfig config;
    config.depth_samples = 32;
    estimateDepth(mvs_views.front(), neighbours, config);
  }});

  // A coarse volume over a short range. The synthetic scene's floor is an
  // infinite plane, so integrating it to the default five metres allocates
  // blocks for a hundred square metres of floor nobody is benchmarking.
  TsdfConfig tsdf_config;
  tsdf_config.voxel_size = 0.02f;
  tsdf_config.truncation = 0.08f;
  tsdf_config.max_depth = 1.6f;
  TsdfVolume volume(tsdf_config);
  const Pose tsdf_pose = SyntheticScene::orbitPoses(Vec3(0.0, 0.3, 0.0), 0.9, 0.25, 1).front();
  cases.push_back({"tsdf.integrate", "frame", 1.0, [&] {
    volume.integrate(f.frame.depth, f.frame.color, f.intrinsics, tsdf_pose);
  }});

  cases.push_back({"splat.forward", "frame", 1.0, [&] {
    RasterOptions options;
    renderSplats(f.cloud, f.splat_view, options);
  }});

  const RasterOptions backward_options;
  const std::vector<ProjectedSplat> projected =
      projectSplats(f.cloud, f.splat_view, backward_options);
  const SplatImage rendered = rasterizeSplats(projected, f.splat_view, backward_options);
  std::vector<float> loss_gradient;
  photometricLoss(rendered, f.target, 0.2f, &loss_gradient);
  cases.push_back({"splat.backward", "frame", 1.0, [&] {
    SplatGradients gradients;
    gradients.resize(f.cloud.size(), 0);
    rasterizeSplatsBackward(f.cloud, f.splat_view, backward_options, projected, rendered,
                            loss_gradient, &gradients);
  }});

  cases.push_back({"splat.preview", "gaussian", static_cast<double>(f.cloud.size()), [&] {
    buildSplatPreview(f.cloud);
  }});

  cases.push_back({"html.viewer", "triangle", static_cast<double>(f.mesh.triangleCount()), [&] {
    HtmlViewerOptions options;
    options.target_triangles = 0;
    buildHtmlViewer(f.mesh, ColorBuffer{}, options);
  }});

  std::printf("%-26s %10s %14s %s\n", "case", "median ms", "per unit", "unit");
  std::printf("%-26s %10s %14s %s\n", "----", "---------", "--------", "----");
  for (const Case& entry : cases) {
    const double milliseconds = medianMilliseconds(entry.run, runs);
    const double per_unit =
        entry.units_per_run > 0.0 ? milliseconds / entry.units_per_run : milliseconds;
    std::printf("%-26s %10.2f %14.4f %s\n", entry.name.c_str(), milliseconds, per_unit,
                entry.unit.c_str());
    std::fflush(stdout);
  }
  return 0;
}
