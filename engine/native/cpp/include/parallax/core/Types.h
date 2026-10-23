// Parallax reconstruction engine — fundamental value types.
//
// The engine is deliberately dependency-light: Eigen (MPL-2.0) is the only
// mandatory third-party library. Everything else (features, SfM, fusion,
// meshing, export) is implemented here so the whole pipeline can be built and
// unit-tested on a host machine as well as cross-compiled for Android.
#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace parallax {

using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Vec4 = Eigen::Vector4d;
using Vec2f = Eigen::Vector2f;
using Vec3f = Eigen::Vector3f;
using Mat3 = Eigen::Matrix3d;
using Mat4 = Eigen::Matrix4d;
using Mat34 = Eigen::Matrix<double, 3, 4>;
using Quat = Eigen::Quaterniond;

/// Pinhole intrinsics with optional radial distortion (Brown-Conrady k1,k2).
struct CameraIntrinsics {
  double fx = 0.0, fy = 0.0;
  double cx = 0.0, cy = 0.0;
  double k1 = 0.0, k2 = 0.0;
  int width = 0, height = 0;

  Mat3 matrix() const {
    Mat3 k = Mat3::Identity();
    k(0, 0) = fx; k(1, 1) = fy; k(0, 2) = cx; k(1, 2) = cy;
    return k;
  }

  /// Project a point in camera space to pixels. Returns false behind the camera.
  bool project(const Vec3& p_cam, Vec2* out_px) const {
    if (p_cam.z() <= 1e-9) return false;
    const double xn = p_cam.x() / p_cam.z();
    const double yn = p_cam.y() / p_cam.z();
    const double r2 = xn * xn + yn * yn;
    const double d = 1.0 + k1 * r2 + k2 * r2 * r2;
    *out_px = Vec2(fx * xn * d + cx, fy * yn * d + cy);
    return true;
  }

  /// Back-project a pixel to a unit-depth ray in camera space (distortion is
  /// undone iteratively; two fixed-point steps are ample for phone lenses).
  Vec3 unproject(const Vec2& px) const {
    double xn = (px.x() - cx) / fx;
    double yn = (px.y() - cy) / fy;
    if (k1 != 0.0 || k2 != 0.0) {
      const double xd = xn, yd = yn;
      for (int i = 0; i < 4; ++i) {
        const double r2 = xn * xn + yn * yn;
        const double d = 1.0 + k1 * r2 + k2 * r2 * r2;
        xn = xd / d;
        yn = yd / d;
      }
    }
    return Vec3(xn, yn, 1.0);
  }

  bool valid() const { return fx > 0.0 && fy > 0.0 && width > 0 && height > 0; }
};

/// A single-channel 8-bit image view onto caller-owned memory.
struct GrayImage {
  const uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  ///< bytes per row; 0 means tightly packed (== width)

  int rowStride() const { return stride > 0 ? stride : width; }
  uint8_t at(int x, int y) const { return data[static_cast<size_t>(y) * rowStride() + x]; }
  bool valid() const { return data != nullptr && width > 1 && height > 1; }
  bool inside(int x, int y, int margin = 0) const {
    return x >= margin && y >= margin && x < width - margin && y < height - margin;
  }
  /// Bilinear sample; caller guarantees the 2x2 neighbourhood is inside.
  double sample(double x, double y) const {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double ax = x - x0, ay = y - y0;
    const double v00 = at(x0, y0), v10 = at(x0 + 1, y0);
    const double v01 = at(x0, y0 + 1), v11 = at(x0 + 1, y0 + 1);
    return (v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay;
  }
};

/// Owning 8-bit single channel image (used by tests, pyramids and tools).
struct GrayBuffer {
  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;

  GrayBuffer() = default;
  GrayBuffer(int w, int h) : pixels(static_cast<size_t>(w) * h, 0), width(w), height(h) {}

  uint8_t& at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
  uint8_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
  GrayImage view() const { return GrayImage{pixels.data(), width, height, width}; }
};

/// RGB image, 3 interleaved bytes per pixel.
struct ColorBuffer {
  std::vector<uint8_t> pixels;  ///< RGB interleaved
  int width = 0;
  int height = 0;

  ColorBuffer() = default;
  ColorBuffer(int w, int h) : pixels(static_cast<size_t>(w) * h * 3, 0), width(w), height(h) {}

  void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    const size_t i = (static_cast<size_t>(y) * width + x) * 3;
    pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b;
  }
  void get(int x, int y, uint8_t* rgb) const {
    const size_t i = (static_cast<size_t>(y) * width + x) * 3;
    rgb[0] = pixels[i]; rgb[1] = pixels[i + 1]; rgb[2] = pixels[i + 2];
  }
  bool empty() const { return pixels.empty(); }
};

/// Float depth map in metres; 0 or NaN means "no measurement".
struct DepthMap {
  std::vector<float> depth;
  std::vector<uint8_t> confidence;  ///< 0..255, may be empty
  int width = 0;
  int height = 0;

  DepthMap() = default;
  DepthMap(int w, int h) : depth(static_cast<size_t>(w) * h, 0.f), width(w), height(h) {}

  float at(int x, int y) const { return depth[static_cast<size_t>(y) * width + x]; }
  float& at(int x, int y) { return depth[static_cast<size_t>(y) * width + x]; }
  uint8_t conf(int x, int y) const {
    if (confidence.empty()) return 255;
    return confidence[static_cast<size_t>(y) * width + x];
  }
  bool empty() const { return depth.empty(); }
};

constexpr double kPi = 3.14159265358979323846;

inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / kPi; }

}  // namespace parallax
