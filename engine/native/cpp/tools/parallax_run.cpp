/**
 * Run the pipeline over a folder of photographs.
 *
 * The app's Import mode is exactly this: a set of images, no device poses, no depth maps, and a
 * pipeline that has to recover both. Every bug the device has reported has been in that chain,
 * and the synthetic captures in the tests cannot find them -- they are rendered by the same
 * code that consumes them, in the same conventions, with no lens distortion, no rolling
 * shutter, no JPEG artefacts and no EXIF to misread. Real photographs find different bugs.
 *
 * Usage:
 *   parallax_run <image-dir> <work-dir> [--mode object|room|area|import]
 *                                       [--preset fast|balanced|high|max]
 *                                       [--stage-only] [--quiet]
 *
 * Exits non-zero on the first stage that fails, having said which and why.
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "parallax/io/ImageIo.h"
#include "parallax/io/JpegIo.h"
#include "parallax/io/Json.h"
#include "parallax/pipeline/Pipeline.h"

namespace fs = std::filesystem;
using namespace parallax;

namespace {

struct Options {
  std::string image_dir;
  std::string work_dir;
  CaptureMode mode = CaptureMode::Room;
  std::string preset = "balanced";
  bool quiet = false;
  /// Use the manifest already in the work directory, poses and all.
  bool keep_manifest = false;
  bool has_poses = false;
};

bool looksLikeImage(const fs::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

/**
 * Guess the focal length from the image size.
 *
 * A photograph carries its focal length in EXIF in millimetres, which is useless without the
 * sensor size, and the sensor size is usually absent. 1.2 times the width is the field of view
 * of a typical phone or compact camera's main lens and is what the pose stage starts from; it
 * refines the intrinsics itself, so the guess only has to be close enough to converge. The app
 * never relies on this -- ARCore reports real intrinsics, and Import reads them from EXIF where
 * they exist -- but a tool that has to work on any folder of photographs does.
 */
CameraIntrinsics guessIntrinsics(int width, int height) {
  CameraIntrinsics k;
  k.width = width;
  k.height = height;
  k.fx = 1.2 * width;
  k.fy = k.fx;
  k.cx = width / 2.0;
  k.cy = height / 2.0;
  return k;
}

bool buildManifest(const Options& options, const std::vector<fs::path>& images, int* frames_out) {
  Json frames = Json::array();
  int index = 0;
  for (const fs::path& source : images) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(source.string(), &bytes)) {
      std::fprintf(stderr, "could not read %s\n", source.string().c_str());
      return false;
    }
    ColorBuffer decoded;
    const bool ok = looksLikeJpeg(bytes) ? decodeJpeg(bytes, &decoded)
                                         : decodePng(bytes, &decoded);
    if (!ok || decoded.empty()) {
      std::fprintf(stderr, "could not decode %s\n", source.string().c_str());
      return false;
    }

    char name[64];
    std::snprintf(name, sizeof(name), "%04d%s", index,
                  source.extension().string().c_str());
    const fs::path destination = fs::path(options.work_dir) / "frames" / name;
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      std::fprintf(stderr, "could not copy %s: %s\n", source.string().c_str(),
                   ec.message().c_str());
      return false;
    }

    const CameraIntrinsics k = guessIntrinsics(decoded.width, decoded.height);
    Json intrinsics = Json::object();
    intrinsics.set("fx", Json::number(k.fx));
    intrinsics.set("fy", Json::number(k.fy));
    intrinsics.set("cx", Json::number(k.cx));
    intrinsics.set("cy", Json::number(k.cy));
    intrinsics.set("width", Json::integer(k.width));
    intrinsics.set("height", Json::integer(k.height));

    Json entry = Json::object();
    entry.set("index", Json::integer(index));
    entry.set("image", Json::string(name));
    entry.set("keyframe", Json::boolean(true));
    entry.set("sharpness", Json::number(420.0));
    entry.set("exposure", Json::number(128.0));
    entry.set("intrinsics", std::move(intrinsics));
    frames.push(std::move(entry));
    ++index;
  }

  Json manifest = Json::object();
  manifest.set("version", Json::integer(1));
  manifest.set("frames", std::move(frames));
  *frames_out = index;
  return writeFileText((fs::path(options.work_dir) / "manifest.json").string(),
                       manifest.dump(2));
}

PipelineConfig configFor(const Options& options) {
  PipelineConfig config;
  config.project_dir = options.work_dir;
  config.mode = options.mode;
  // No device poses and no depth: structure from motion, then multi-view stereo. This is the
  // Import path, and the one with no hardware in it at all.
  config.has_device_poses = options.has_poses;
  config.has_depth_maps = false;

  if (options.preset == "fast") {
    config.voxel_size = 0.03f;
    config.max_keyframes = 40;
    config.target_triangles = 60000;
    config.texture_resolution = 1024;
  } else if (options.preset == "high") {
    config.voxel_size = 0.008f;
    config.max_keyframes = 160;
    config.target_triangles = 400000;
    config.texture_resolution = 4096;
  } else if (options.preset == "max") {
    config.voxel_size = 0.005f;
    config.max_keyframes = 240;
    config.target_triangles = 800000;
    config.texture_resolution = 4096;
  } else {
    config.voxel_size = 0.015f;
    config.max_keyframes = 80;
    config.target_triangles = 200000;
    config.texture_resolution = 2048;
  }

  switch (options.mode) {
    case CaptureMode::Object:
      config.max_integration_depth = 3.0f;
      break;
    case CaptureMode::Room:
    case CaptureMode::Area:
      // A facade or a room needs the range; the voxel budget is what limits it.
      config.max_integration_depth = 8.0f;
      break;
    case CaptureMode::Import:
      config.max_integration_depth = 6.0f;
      break;
  }
  return config;
}

bool parse(int argc, char** argv, Options* options) {
  if (argc < 3) return false;
  options->image_dir = argv[1];
  options->work_dir = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quiet") {
      options->quiet = true;
    } else if (arg == "--keep-manifest") {
      // For a project directory built elsewhere -- a dataset with real camera poses, say,
      // which is the only way to exercise the path a device capture takes.
      options->keep_manifest = true;
      options->has_poses = true;
    } else if (arg == "--mode" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "object") options->mode = CaptureMode::Object;
      else if (mode == "room") options->mode = CaptureMode::Room;
      else if (mode == "area" || mode == "outdoor") options->mode = CaptureMode::Area;
      else if (mode == "import") options->mode = CaptureMode::Import;
      else return false;
    } else if (arg == "--preset" && i + 1 < argc) {
      options->preset = argv[++i];
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, &options)) {
    std::fprintf(stderr,
                 "usage: parallax_run <image-dir> <work-dir> [--mode object|room|area|import]"
                 " [--preset fast|balanced|high|max] [--quiet] [--keep-manifest]\n");
    return 2;
  }

  std::vector<fs::path> images;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(options.image_dir, ec)) {
    if (entry.is_regular_file() && looksLikeImage(entry.path())) images.push_back(entry.path());
  }
  if (ec || images.empty()) {
    std::fprintf(stderr, "no images in %s\n", options.image_dir.c_str());
    return 2;
  }
  std::sort(images.begin(), images.end());

  fs::create_directories(options.work_dir, ec);
  int frame_count = static_cast<int>(images.size());
  if (options.keep_manifest) {
    std::printf("using the manifest already in %s\n", options.work_dir.c_str());
  } else {
    if (!buildManifest(options, images, &frame_count)) return 2;
    std::printf("%d frames from %s\n", frame_count, options.image_dir.c_str());
  }

  Pipeline pipeline(configFor(options));
  const auto started = std::chrono::steady_clock::now();
  while (!pipeline.isComplete()) {
    const Stage stage = pipeline.nextStage();
    const auto stage_started = std::chrono::steady_clock::now();
    ProgressCallback report;
    if (!options.quiet) {
      report = [](const Progress& progress) {
        std::printf("\r    %5.1f%%  %-58s", progress.stage_fraction * 100.0,
                    progress.detail.c_str());
        std::fflush(stdout);
      };
    }
    const StageOutcome outcome = pipeline.runStage(stage, report, nullptr);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - stage_started)
                               .count();
    if (!options.quiet) std::printf("\r%-70s\r", "");
    std::printf("%-14s %6.1fs  %s\n", stageName(stage), seconds, outcome.message.c_str());
    if (!outcome.success) {
      std::printf("\nFAILED at %s: %s\n", stageName(stage), outcome.message.c_str());
      return 1;
    }
  }

  const double total = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  const PipelineSummary& summary = pipeline.summary();
  std::printf("\ncomplete in %.1fs\n", total);
  std::printf("  frames      %d\n", summary.frame_count);
  std::printf("  triangles   %d\n", summary.triangle_count);
  std::printf("  points      %d\n", summary.point_count);
  std::printf("  extent      %.2f m\n", summary.bounds.diagonal());
  return 0;
}
