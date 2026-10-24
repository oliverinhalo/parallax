// JNI bridge for the Parallax reconstruction engine.
//
// Only this file knows about JNI; everything under src/ is plain C++ so the whole engine can be
// built and unit-tested on a host machine. The bridge is deliberately thin and coarse-grained:
// it hands over paths and configuration, runs whole pipeline stages, and marshals results.
// A chatty per-frame or per-vertex interface would spend more time crossing the boundary than
// doing work, and would make the engine untestable without a JVM.
#include <jni.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "parallax/exporters/MeshExport.h"
#include "parallax/features/Features.h"
#include "parallax/io/ImageIo.h"
#include "parallax/io/Json.h"
#include "parallax/io/MeshReader.h"
#include "parallax/pipeline/Pipeline.h"
#include "parallax/splat/GaussianCloud.h"

#ifdef __ANDROID__
#include <android/log.h>
#define PARALLAX_LOG(...) __android_log_print(ANDROID_LOG_INFO, "ParallaxEngine", __VA_ARGS__)
#else
#define PARALLAX_LOG(...) ((void)0)
#endif

namespace {

using namespace parallax;

/// Live pipeline instances, keyed by an opaque handle.
///
/// Handles rather than raw pointers because the Kotlin side can be killed and restarted at any
/// moment: an integer that is simply absent from the table after a restart produces a clean
/// error, whereas a stale pointer cast back from a jlong is a crash.
struct Registry {
  std::mutex mutex;
  int64_t next_handle = 1;
  std::unordered_map<int64_t, std::unique_ptr<Pipeline>> pipelines;
  std::unordered_map<int64_t, CancelToken> cancels;
};

Registry& registry() {
  static Registry instance;
  return instance;
}

std::string toStdString(JNIEnv* env, jstring value) {
  if (value == nullptr) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) return {};
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

jstring toJavaString(JNIEnv* env, const std::string& value) {
  return env->NewStringUTF(value.c_str());
}

/// Parse the configuration JSON the Kotlin side sends.
///
/// JSON rather than a long argument list or a mirrored struct: the configuration has twenty-odd
/// fields that change as the pipeline evolves, and a positional signature would have to be
/// edited in two languages every time, with silent misalignment as the failure mode.
PipelineConfig parseConfig(const std::string& json_text, bool* ok) {
  PipelineConfig config;
  Json json;
  std::string error;
  *ok = Json::parse(json_text, &json, &error) && json.isObject();
  if (!*ok) return config;

  config.project_dir = json.get("projectDir").asString();
  const std::string mode = json.get("mode").asString();
  if (mode == "room") {
    config.mode = CaptureMode::Room;
  } else if (mode == "area") {
    config.mode = CaptureMode::Area;
  } else if (mode == "import") {
    config.mode = CaptureMode::Import;
  } else {
    config.mode = CaptureMode::Object;
  }

  config.voxel_size = static_cast<float>(json.get("voxelSize").asDouble(0.008));
  config.truncation_multiplier =
      static_cast<float>(json.get("truncationMultiplier").asDouble(4.0));
  config.max_keyframes = static_cast<int>(json.get("maxKeyframes").asInt(160));
  config.texture_resolution = static_cast<int>(json.get("textureResolution").asInt(4096));
  config.target_triangles = static_cast<int>(json.get("targetTriangles").asInt(300000));
  config.has_device_poses = json.get("hasDevicePoses").asBool(true);
  config.has_depth_maps = json.get("hasDepthMaps").asBool(true);
  config.memory_budget_bytes =
      static_cast<size_t>(json.get("memoryBudgetBytes").asInt(512ll * 1024 * 1024));
  config.max_integration_depth =
      static_cast<float>(json.get("maxIntegrationDepth").asDouble(3.0));
  config.remove_ground_plane = json.get("removeGroundPlane").asBool(false);
  config.keep_largest_component = json.get("keepLargestComponent").asBool(false);
  config.auto_level = json.get("autoLevel").asBool(true);
  config.build_point_cloud = json.get("buildPointCloud").asBool(true);
  config.build_watertight = json.get("buildWatertight").asBool(true);
  config.analyse_room = json.get("analyseRoom").asBool(false);
  config.train_splats = json.get("trainSplats").asBool(false);
  config.splat_iterations = static_cast<int>(json.get("splatIterations").asInt(5000));
  config.splat_max_gaussians = static_cast<int>(json.get("splatMaxGaussians").asInt(180000));
  config.splat_sh_degree = static_cast<int>(json.get("splatShDegree").asInt(2));
  config.splat_image_width = static_cast<int>(json.get("splatImageWidth").asInt(512));
  config.splat_max_views = static_cast<int>(json.get("splatMaxViews").asInt(80));

  *ok = !config.project_dir.empty();
  return config;
}

std::string summaryToJson(const PipelineSummary& summary) {
  Json json = Json::object();
  json.set("frameCount", Json::integer(summary.frame_count));
  json.set("keyframeCount", Json::integer(summary.keyframe_count));
  json.set("registeredFrameCount", Json::integer(summary.registered_frame_count));
  json.set("meanReprojectionErrorPx", Json::number(summary.mean_reprojection_error_px));
  json.set("triangleCount", Json::integer(summary.triangle_count));
  json.set("vertexCount", Json::integer(summary.vertex_count));
  json.set("pointCount", Json::integer(summary.point_count));
  json.set("splatCount", Json::integer(summary.splat_count));
  json.set("splatBackend", Json::string(summary.splat_backend));
  json.set("surfaceAreaM2", Json::number(summary.surface_area_m2));
  json.set("volumeM3", Json::number(summary.volume_m3));
  json.set("watertight", Json::boolean(summary.watertight));
  json.set("floorAreaM2", Json::number(summary.floor_area_m2));
  json.set("ceilingHeightM", Json::number(summary.ceiling_height_m));
  json.set("hasFloorPlan", Json::boolean(summary.has_floor_plan));
  json.set("hasTexture", Json::boolean(summary.has_texture));
  json.set("scaleFactor", Json::number(summary.scale_factor));
  if (summary.bounds.valid()) {
    Json bounds = Json::array();
    for (int i = 0; i < 3; ++i) bounds.push(Json::number(summary.bounds.min[i]));
    for (int i = 0; i < 3; ++i) bounds.push(Json::number(summary.bounds.max[i]));
    json.set("bounds", std::move(bounds));
  }
  return json.dump();
}

std::string outcomeToJson(const StageOutcome& outcome, const PipelineSummary& summary,
                          Stage next) {
  Json json = Json::object();
  json.set("success", Json::boolean(outcome.success));
  json.set("cancelled", Json::boolean(outcome.cancelled));
  json.set("message", Json::string(outcome.message));
  json.set("nextStage", Json::integer(static_cast<int>(next)));
  json.set("complete", Json::boolean(next == Stage::Count));
  Json summary_json;
  Json::parse(summaryToJson(summary), &summary_json);
  json.set("summary", std::move(summary_json));
  return json.dump();
}

/// Build a progress callback that invokes `ProgressListener.onProgress` on the Java side.
struct ProgressBridge {
  JNIEnv* env = nullptr;
  jobject listener = nullptr;
  jmethodID method = nullptr;

  bool bind(JNIEnv* e, jobject l) {
    env = e;
    listener = l;
    if (listener == nullptr) return false;
    jclass clazz = env->GetObjectClass(listener);
    if (clazz == nullptr) return false;
    method = env->GetMethodID(clazz, "onProgress", "(IDDLjava/lang/String;)V");
    env->DeleteLocalRef(clazz);
    return method != nullptr;
  }

  void operator()(const Progress& progress) const {
    if (method == nullptr) return;
    jstring detail = env->NewStringUTF(progress.detail.c_str());
    env->CallVoidMethod(listener, method, static_cast<jint>(progress.stage),
                        static_cast<jdouble>(progress.stage_fraction),
                        static_cast<jdouble>(progress.overall_fraction), detail);
    if (detail != nullptr) env->DeleteLocalRef(detail);
    // Swallowing a pending Java exception here would hide a listener bug; clear it so the
    // native loop can continue and report its own outcome, having logged it.
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }
};

ExportOptions parseExportOptions(const std::string& json_text) {
  ExportOptions options;
  Json json;
  if (!Json::parse(json_text, &json) || !json.isObject()) return options;

  const std::string unit = json.get("unit").asString();
  if (unit == "cm") {
    options.unit = LengthUnit::Centimetres;
  } else if (unit == "mm") {
    options.unit = LengthUnit::Millimetres;
  } else if (unit == "in") {
    options.unit = LengthUnit::Inches;
  } else if (unit == "ft") {
    options.unit = LengthUnit::Feet;
  }
  options.up_axis = json.get("upAxis").asString() == "z" ? UpAxis::ZUp : UpAxis::YUp;
  options.handedness = json.get("handedness").asString() == "left" ? Handedness::LeftHanded
                                                                   : Handedness::RightHanded;
  options.max_texture_size = static_cast<int>(json.get("maxTextureSize").asInt(4096));
  options.target_triangles = static_cast<int>(json.get("targetTriangles").asInt(0));
  options.binary = json.get("binary").asBool(true);
  options.include_vertex_colors = json.get("includeVertexColors").asBool(true);
  options.include_normals = json.get("includeNormals").asBool(true);
  options.include_uvs = json.get("includeUvs").asBool(true);
  if (json.has("name")) options.name = json.get("name").asString();
  return options;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeVersion(JNIEnv* env,
                                                                              jclass) {
  return toJavaString(env, "parallax-engine 1.0.0");
}

// ------------------------------------------------------------------- pipeline

JNIEXPORT jlong JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeCreatePipeline(
    JNIEnv* env, jclass, jstring config_json) {
  bool ok = false;
  const PipelineConfig config = parseConfig(toStdString(env, config_json), &ok);
  if (!ok) return 0;

  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const int64_t handle = reg.next_handle++;
  reg.pipelines[handle] = std::make_unique<Pipeline>(config);
  reg.cancels[handle] = std::make_shared<std::atomic_bool>(false);
  PARALLAX_LOG("created pipeline %lld for %s", static_cast<long long>(handle),
               config.project_dir.c_str());
  return static_cast<jlong>(handle);
}

JNIEXPORT void JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeDestroyPipeline(
    JNIEnv*, jclass, jlong handle) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.pipelines.erase(static_cast<int64_t>(handle));
  reg.cancels.erase(static_cast<int64_t>(handle));
}

JNIEXPORT jint JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeNextStage(JNIEnv*, jclass,
                                                                                jlong handle) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
  if (it == reg.pipelines.end()) return -1;
  return static_cast<jint>(it->second->nextStage());
}

JNIEXPORT void JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeCancel(JNIEnv*, jclass,
                                                                             jlong handle) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = reg.cancels.find(static_cast<int64_t>(handle));
  if (it != reg.cancels.end()) it->second->store(true, std::memory_order_relaxed);
}

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeRunStage(
    JNIEnv* env, jclass, jlong handle, jint stage, jobject listener) {
  Pipeline* pipeline = nullptr;
  CancelToken cancel;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) {
      return toJavaString(
          env, R"({"success":false,"cancelled":false,"message":"unknown pipeline handle")"
               R"(,"nextStage":-1,"complete":false})");
    }
    pipeline = it->second.get();
    cancel = reg.cancels[static_cast<int64_t>(handle)];
    // A fresh run must not inherit a cancellation from the previous one.
    if (cancel) cancel->store(false, std::memory_order_relaxed);
  }
  // The pipeline is deliberately run with the registry lock released: a stage takes minutes,
  // and holding the lock would block cancellation, which is the one call that must get through.
  if (stage < 0 || stage >= static_cast<jint>(Stage::Count)) {
    return toJavaString(env,
                        R"({"success":false,"cancelled":false,"message":"stage out of range")"
                        R"(,"nextStage":-1,"complete":false})");
  }

  ProgressBridge bridge;
  const bool has_listener = bridge.bind(env, listener);
  const StageOutcome outcome =
      pipeline->runStage(static_cast<Stage>(stage),
                         has_listener ? ProgressCallback(bridge) : ProgressCallback(), cancel);
  return toJavaString(env, outcomeToJson(outcome, pipeline->summary(), pipeline->nextStage()));
}

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeRunToCompletion(
    JNIEnv* env, jclass, jlong handle, jobject listener) {
  Pipeline* pipeline = nullptr;
  CancelToken cancel;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) {
      return toJavaString(
          env, R"({"success":false,"cancelled":false,"message":"unknown pipeline handle")"
               R"(,"nextStage":-1,"complete":false})");
    }
    pipeline = it->second.get();
    cancel = reg.cancels[static_cast<int64_t>(handle)];
    if (cancel) cancel->store(false, std::memory_order_relaxed);
  }
  ProgressBridge bridge;
  const bool has_listener = bridge.bind(env, listener);
  const StageOutcome outcome = pipeline->runToCompletion(
      has_listener ? ProgressCallback(bridge) : ProgressCallback(), cancel);
  return toJavaString(env, outcomeToJson(outcome, pipeline->summary(), pipeline->nextStage()));
}

JNIEXPORT void JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeInvalidateFrom(
    JNIEnv*, jclass, jlong handle, jint stage) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
  if (it == reg.pipelines.end()) return;
  if (stage < 0 || stage >= static_cast<jint>(Stage::Count)) return;
  it->second->invalidateFrom(static_cast<Stage>(stage));
}

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeSummary(JNIEnv* env,
                                                                               jclass,
                                                                               jlong handle) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
  if (it == reg.pipelines.end()) return toJavaString(env, "{}");
  return toJavaString(env, summaryToJson(it->second->summary()));
}

// --------------------------------------------------------------------- export

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeExport(
    JNIEnv* env, jclass, jlong handle, jstring format, jstring output_path,
    jstring options_json) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) {
      return toJavaString(env, R"({"success":false,"message":"unknown pipeline handle"})");
    }
    pipeline = it->second.get();
  }

  const std::string kind = toStdString(env, format);
  const std::string path = toStdString(env, output_path);
  const ExportOptions options = parseExportOptions(toStdString(env, options_json));

  const auto fail = [env](const std::string& message) {
    Json json = Json::object();
    json.set("success", Json::boolean(false));
    json.set("message", Json::string(message));
    return toJavaString(env, json.dump());
  };
  if (path.empty()) return fail("no output path");

  bool success = false;
  std::string message;

  if (kind == "ply_points") {
    std::vector<Vec3f> points, normals;
    std::vector<std::array<uint8_t, 3>> colors;
    if (!pipeline->loadPointCloud(&points, &normals, &colors)) {
      return fail("this project has no point cloud");
    }
    success = writePointCloudPly(path, points, normals, colors, options);
    message = success ? std::to_string(points.size()) + " points" : "write failed";
  } else {
    Mesh mesh = pipeline->loadMesh();
    if (mesh.empty()) return fail("this project has no mesh");
    const ColorBuffer atlas = pipeline->loadAtlas();

    if (kind == "glb") {
      success = writeGlb(path, mesh, atlas, options);
    } else if (kind == "gltf") {
      // `path` is the directory for the multi-file layout; the base name follows the model.
      success = writeGltf(path, options.name, mesh, atlas, options);
    } else if (kind == "obj") {
      success = writeObj(path, options.name, mesh, atlas, options);
    } else if (kind == "usdz") {
      success = writeUsdz(path, mesh, atlas, options);
    } else if (kind == "ply") {
      success = writePly(path, mesh, options);
    } else if (kind == "stl") {
      // STL must be watertight to print, so prefer the sealed variant when one exists.
      Mesh watertight = mesh;
      fillHoles(&watertight, 1000000);
      success = writeStl(path, watertight, options);
      message = isWatertight(watertight) ? "watertight" : "not fully watertight";
    } else {
      return fail("unsupported format: " + kind);
    }
    if (message.empty()) {
      message = success ? std::to_string(mesh.triangleCount()) + " triangles" : "write failed";
    }
  }

  Json json = Json::object();
  json.set("success", Json::boolean(success));
  json.set("message", Json::string(message));
  return toJavaString(env, json.dump());
}

// --------------------------------------------------- live capture helpers

/**
 * Score a preview frame for sharpness and exposure.
 *
 * Takes a direct ByteBuffer wrapping the camera's Y plane, so the luma data is read in place
 * with no copy and no JNI array pinning -- this runs on every preview frame, and a copy of a
 * multi-megapixel plane at 30 Hz would be the most expensive thing in the app.
 *
 * Returns the five measured values packed into a double array, rather than allocating an object
 * per frame.
 */
JNIEXPORT jdoubleArray JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeScoreFrame(
    JNIEnv* env, jclass, jobject luma_buffer, jint width, jint height, jint row_stride) {
  jdoubleArray result = env->NewDoubleArray(6);
  if (result == nullptr) return nullptr;

  const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(luma_buffer));
  const jlong capacity = env->GetDirectBufferCapacity(luma_buffer);
  double values[6] = {0, 0, 0, 0, 0, 0};

  if (data != nullptr && width > 1 && height > 1 && row_stride >= width &&
      capacity >= static_cast<jlong>(row_stride) * height) {
    GrayImage image;
    image.data = data;
    image.width = width;
    image.height = height;
    image.stride = row_stride;
    const FrameQuality quality = scoreFrame(image);
    values[0] = quality.sharpness;
    values[1] = quality.exposure;
    values[2] = quality.contrast;
    values[3] = quality.clipped_low;
    values[4] = quality.clipped_high;
    values[5] = quality.acceptable ? 1.0 : 0.0;
  }
  env->SetDoubleArrayRegion(result, 0, 6, values);
  return result;
}

/// Perceptual hash of a preview frame, for near-duplicate rejection during capture.
JNIEXPORT jlong JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeFrameHash(
    JNIEnv* env, jclass, jobject luma_buffer, jint width, jint height, jint row_stride) {
  const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(luma_buffer));
  const jlong capacity = env->GetDirectBufferCapacity(luma_buffer);
  if (data == nullptr || width <= 1 || height <= 1 || row_stride < width ||
      capacity < static_cast<jlong>(row_stride) * height) {
    return 0;
  }
  GrayImage image;
  image.data = data;
  image.width = width;
  image.height = height;
  image.stride = row_stride;
  return static_cast<jlong>(differenceHash(image));
}

/// Hamming distance between two frame hashes, so the capture loop need not reimplement popcount.
JNIEXPORT jint JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeHashDistance(
    JNIEnv*, jclass, jlong a, jlong b) {
  return static_cast<jint>(
      hammingDistance64(static_cast<uint64_t>(a), static_cast<uint64_t>(b)));
}

// ------------------------------------------------------------------- loading

/**
 * Load the finished mesh as flat arrays for the renderer.
 *
 * Returns a float array laid out as a header followed by interleaved vertex data, and fills the
 * caller's index array. One call and two allocations beats a getter per attribute.
 */
JNIEXPORT jfloatArray JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeLoadMeshVertices(
    JNIEnv* env, jclass, jlong handle) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) return nullptr;
    pipeline = it->second.get();
  }
  const Mesh mesh = pipeline->loadMesh();
  if (mesh.empty()) return nullptr;

  // position(3) + normal(3) + uv(2) + colour(3) = 11 floats per vertex. Colour is normalised
  // here so the shader does not need a separate integer attribute.
  const int stride = 11;
  jfloatArray array = env->NewFloatArray(static_cast<jsize>(mesh.vertexCount() * stride));
  if (array == nullptr) return nullptr;
  std::vector<float> data(mesh.vertexCount() * stride, 0.f);
  for (size_t i = 0; i < mesh.vertexCount(); ++i) {
    float* v = data.data() + i * stride;
    v[0] = mesh.vertices[i].x();
    v[1] = mesh.vertices[i].y();
    v[2] = mesh.vertices[i].z();
    if (mesh.hasNormals()) {
      v[3] = mesh.normals[i].x();
      v[4] = mesh.normals[i].y();
      v[5] = mesh.normals[i].z();
    } else {
      v[4] = 1.f;
    }
    if (mesh.hasUvs()) {
      v[6] = mesh.uvs[i].x();
      v[7] = mesh.uvs[i].y();
    }
    if (mesh.hasColors()) {
      v[8] = mesh.colors[i][0] / 255.f;
      v[9] = mesh.colors[i][1] / 255.f;
      v[10] = mesh.colors[i][2] / 255.f;
    } else {
      v[8] = v[9] = v[10] = 0.8f;
    }
  }
  env->SetFloatArrayRegion(array, 0, static_cast<jsize>(data.size()), data.data());
  return array;
}

JNIEXPORT jintArray JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeLoadMeshIndices(
    JNIEnv* env, jclass, jlong handle) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) return nullptr;
    pipeline = it->second.get();
  }
  const Mesh mesh = pipeline->loadMesh();
  if (mesh.empty()) return nullptr;
  jintArray array = env->NewIntArray(static_cast<jsize>(mesh.indices.size()));
  if (array == nullptr) return nullptr;
  // uint32 and int32 have the same width; a mesh with more than 2^31 indices is not
  // representable in a Java array anyway, and the exporters cap well below that.
  env->SetIntArrayRegion(array, 0, static_cast<jsize>(mesh.indices.size()),
                         reinterpret_cast<const jint*>(mesh.indices.data()));
  return array;
}

/// Point cloud as interleaved position(3) + normal(3) + colour(3) floats.
JNIEXPORT jfloatArray JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeLoadPointCloud(
    JNIEnv* env, jclass, jlong handle) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) return nullptr;
    pipeline = it->second.get();
  }
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  if (!pipeline->loadPointCloud(&points, &normals, &colors) || points.empty()) return nullptr;

  const int stride = 9;
  std::vector<float> data(points.size() * stride, 0.f);
  const bool has_normals = normals.size() == points.size();
  const bool has_colors = colors.size() == points.size();
  for (size_t i = 0; i < points.size(); ++i) {
    float* v = data.data() + i * stride;
    v[0] = points[i].x();
    v[1] = points[i].y();
    v[2] = points[i].z();
    if (has_normals) {
      v[3] = normals[i].x();
      v[4] = normals[i].y();
      v[5] = normals[i].z();
    } else {
      v[4] = 1.f;
    }
    if (has_colors) {
      v[6] = colors[i][0] / 255.f;
      v[7] = colors[i][1] / 255.f;
      v[8] = colors[i][2] / 255.f;
    } else {
      v[6] = v[7] = v[8] = 0.8f;
    }
  }
  jfloatArray array = env->NewFloatArray(static_cast<jsize>(data.size()));
  if (array == nullptr) return nullptr;
  env->SetFloatArrayRegion(array, 0, static_cast<jsize>(data.size()), data.data());
  return array;
}

/// The floor plan, as JSON the Kotlin side deserialises.
JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeLoadFloorPlan(
    JNIEnv* env, jclass, jlong handle) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) return toJavaString(env, "{}");
    pipeline = it->second.get();
  }
  const FloorPlan plan = pipeline->loadFloorPlan();
  Json json = Json::object();
  json.set("valid", Json::boolean(plan.valid()));
  json.set("floorAreaM2", Json::number(plan.floor_area));
  json.set("perimeterM", Json::number(plan.perimeter));
  json.set("roomHeightM", Json::number(plan.room_height));
  Json outline = Json::array();
  for (const Vec2d& point : plan.outline) {
    Json pair = Json::array();
    pair.push(Json::number(point.x()));
    pair.push(Json::number(point.y()));
    outline.push(std::move(pair));
  }
  json.set("outline", std::move(outline));
  Json walls = Json::array();
  for (const WallSegment& wall : plan.walls) {
    Json entry = Json::object();
    entry.set("lengthM", Json::number(wall.length()));
    walls.push(std::move(entry));
  }
  json.set("walls", std::move(walls));
  return toJavaString(env, json.dump());
}

/// Render the floor plan to an SVG string, for sharing and for the in-app preview.
JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeRenderFloorPlanSvg(
    JNIEnv* env, jclass, jlong handle, jboolean dark, jboolean imperial, jstring title) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) return toJavaString(env, "");
    pipeline = it->second.get();
  }
  const FloorPlan plan = pipeline->loadFloorPlan();
  FloorPlanStyle style;
  style.dark = dark == JNI_TRUE;
  style.unit = imperial == JNI_TRUE ? LengthUnit::Feet : LengthUnit::Metres;
  style.title = toStdString(env, title);
  return toJavaString(env, renderFloorPlanSvg(plan, style));
}

// --------------------------------------------------------------------- splats

/// Initialise a splat cloud from the project's point cloud and write it as a 3DGS PLY.
JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeInitialiseSplats(
    JNIEnv* env, jclass, jlong handle, jstring output_path, jint sh_degree) {
  Pipeline* pipeline = nullptr;
  {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.pipelines.find(static_cast<int64_t>(handle));
    if (it == reg.pipelines.end()) {
      return toJavaString(env, R"({"success":false,"message":"unknown pipeline handle"})");
    }
    pipeline = it->second.get();
  }
  std::vector<Vec3f> points, normals;
  std::vector<std::array<uint8_t, 3>> colors;
  Json json = Json::object();
  if (!pipeline->loadPointCloud(&points, &normals, &colors) || points.empty()) {
    json.set("success", Json::boolean(false));
    json.set("message", Json::string("this project has no point cloud to initialise from"));
    return toJavaString(env, json.dump());
  }
  const GaussianCloud cloud = initializeFromPointCloud(points, colors, sh_degree);
  const bool ok = writeGaussianPly(toStdString(env, output_path), cloud);
  json.set("success", Json::boolean(ok));
  json.set("count", Json::integer(static_cast<int64_t>(cloud.size())));
  json.set("message", Json::string(ok ? "initialised" : "write failed"));
  return toJavaString(env, json.dump());
}

/// Convert a 3DGS PLY to one of the compact splat formats.
JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeConvertSplats(
    JNIEnv* env, jclass, jstring input_path, jstring output_path, jstring format) {
  GaussianCloud cloud;
  std::string error;
  Json json = Json::object();
  if (!readGaussianPly(toStdString(env, input_path), &cloud, &error)) {
    json.set("success", Json::boolean(false));
    json.set("message", Json::string(error.empty() ? "could not read the splat file" : error));
    return toJavaString(env, json.dump());
  }
  const std::string kind = toStdString(env, format);
  const std::string out = toStdString(env, output_path);
  bool ok = false;
  if (kind == "splat") {
    ok = writeSplat(out, cloud);
  } else if (kind == "spz") {
    ok = writeSpz(out, cloud);
  } else if (kind == "ply") {
    ok = writeGaussianPly(out, cloud);
  } else {
    json.set("success", Json::boolean(false));
    json.set("message", Json::string("unsupported splat format: " + kind));
    return toJavaString(env, json.dump());
  }
  json.set("success", Json::boolean(ok));
  json.set("count", Json::integer(static_cast<int64_t>(cloud.size())));
  json.set("message", Json::string(ok ? "converted" : "write failed"));
  return toJavaString(env, json.dump());
}

}  // extern "C"
