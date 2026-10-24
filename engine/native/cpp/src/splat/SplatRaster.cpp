#include "parallax/splat/SplatRaster.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace parallax {
namespace {

/// Real spherical-harmonic normalisation constants, bands 0 to 3.
constexpr float kShC1 = 0.4886025119029199f;
constexpr float kShC2[5] = {1.0925484305920792f, -1.0925484305920792f, 0.31539156525252005f,
                            -1.0925484305920792f, 0.5462742152960396f};
constexpr float kShC3[7] = {-0.5900435899266435f, 2.890611442640554f,  -0.4570457994644658f,
                            0.3731763325901154f,  -0.4570457994644658f, 1.445305721320277f,
                            -0.5900435899266435f};

constexpr int kTileSize = 16;
/// Alpha is capped below one so that a single gaussian can never make a pixel
/// opaque; without the cap the transmittance divide in the backward pass is a
/// division by zero.
constexpr float kMaxAlpha = 0.99f;
/// How far outside the frustum the projection Jacobian is still evaluated.
/// Past this the affine approximation diverges and the splat would smear
/// across the whole image, so the point is clamped onto the boundary instead.
constexpr float kFrustumSlack = 1.3f;

/// Evaluate the 15 non-constant SH basis functions for a direction.
int shBasis(int degree, const Vec3f& d, float* basis) {
  if (degree < 1) return 0;
  const float x = d.x(), y = d.y(), z = d.z();
  basis[0] = -kShC1 * y;
  basis[1] = kShC1 * z;
  basis[2] = -kShC1 * x;
  if (degree < 2) return 3;
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, yz = y * z, xz = x * z;
  basis[3] = kShC2[0] * xy;
  basis[4] = kShC2[1] * yz;
  basis[5] = kShC2[2] * (2.f * zz - xx - yy);
  basis[6] = kShC2[3] * xz;
  basis[7] = kShC2[4] * (xx - yy);
  if (degree < 3) return 8;
  basis[8] = kShC3[0] * y * (3.f * xx - yy);
  basis[9] = kShC3[1] * xy * z;
  basis[10] = kShC3[2] * y * (4.f * zz - xx - yy);
  basis[11] = kShC3[3] * z * (2.f * zz - 3.f * xx - 3.f * yy);
  basis[12] = kShC3[4] * x * (4.f * zz - xx - yy);
  basis[13] = kShC3[5] * z * (xx - yy);
  basis[14] = kShC3[6] * x * (xx - 3.f * yy);
  return 15;
}

/// Derivatives of those basis functions with respect to the direction.
void shBasisJacobian(int degree, const Vec3f& d, int count, Vec3f* jacobian) {
  if (count < 1) return;
  const float x = d.x(), y = d.y(), z = d.z();
  jacobian[0] = Vec3f(0.f, -kShC1, 0.f);
  jacobian[1] = Vec3f(0.f, 0.f, kShC1);
  jacobian[2] = Vec3f(-kShC1, 0.f, 0.f);
  if (count < 8) return;
  const float xx = x * x, yy = y * y, zz = z * z;
  jacobian[3] = kShC2[0] * Vec3f(y, x, 0.f);
  jacobian[4] = kShC2[1] * Vec3f(0.f, z, y);
  jacobian[5] = kShC2[2] * Vec3f(-2.f * x, -2.f * y, 4.f * z);
  jacobian[6] = kShC2[3] * Vec3f(z, 0.f, x);
  jacobian[7] = kShC2[4] * Vec3f(2.f * x, -2.f * y, 0.f);
  if (count < 15) return;
  jacobian[8] = kShC3[0] * Vec3f(6.f * x * y, 3.f * xx - 3.f * yy, 0.f);
  jacobian[9] = kShC3[1] * Vec3f(y * z, x * z, x * y);
  jacobian[10] = kShC3[2] * Vec3f(-2.f * x * y, 4.f * zz - xx - 3.f * yy, 8.f * y * z);
  jacobian[11] = kShC3[3] * Vec3f(-6.f * x * z, -6.f * y * z, 6.f * zz - 3.f * xx - 3.f * yy);
  jacobian[12] = kShC3[4] * Vec3f(4.f * zz - 3.f * xx - yy, -2.f * x * y, 8.f * x * z);
  jacobian[13] = kShC3[5] * Vec3f(2.f * x * z, -2.f * y * z, xx - yy);
  jacobian[14] = kShC3[6] * Vec3f(3.f * xx - 3.f * yy, -6.f * x * y, 0.f);
}

Eigen::Matrix3f rotationOf(const Gaussian& gaussian) {
  return gaussian.rotation.normalized().toRotationMatrix();
}

Eigen::Matrix3f covariance3d(const Gaussian& gaussian) {
  const Eigen::Matrix3f R = rotationOf(gaussian);
  const Vec3f scale = gaussian.log_scale.array().exp();
  const Eigen::Matrix3f M = R * scale.asDiagonal();
  return M * M.transpose();
}

/// The 2x3 Jacobian of the pinhole projection at a camera-space point, with
/// the point clamped into the widened frustum first.
Eigen::Matrix<float, 2, 3> projectionJacobian(const Vec3f& p_cam, float fx, float fy,
                                              float tan_half_x, float tan_half_y) {
  const float z = p_cam.z();
  const float limit_x = tan_half_x * kFrustumSlack;
  const float limit_y = tan_half_y * kFrustumSlack;
  const float x = std::clamp(p_cam.x() / z, -limit_x, limit_x) * z;
  const float y = std::clamp(p_cam.y() / z, -limit_y, limit_y) * z;
  Eigen::Matrix<float, 2, 3> J;
  J << fx / z, 0.f, -fx * x / (z * z),
       0.f, fy / z, -fy * y / (z * z);
  return J;
}

struct TileGrid {
  int cols = 0;
  int rows = 0;
  /// Flat list of splat indices, tile by tile, each span already near-to-far.
  std::vector<int> entries;
  std::vector<int> offsets;  ///< cols*rows + 1 entries

  int tileCount() const { return cols * rows; }
};

TileGrid buildTiles(const std::vector<ProjectedSplat>& projected, int width, int height) {
  TileGrid grid;
  grid.cols = (width + kTileSize - 1) / kTileSize;
  grid.rows = (height + kTileSize - 1) / kTileSize;
  const int tiles = grid.tileCount();
  std::vector<int> counts(static_cast<size_t>(tiles) + 1, 0);

  const auto tileRange = [&](const ProjectedSplat& s, int* x0, int* x1, int* y0, int* y1) {
    *x0 = std::max(0, static_cast<int>(std::floor((s.mean.x() - s.radius) / kTileSize)));
    *x1 = std::min(grid.cols - 1, static_cast<int>(std::floor((s.mean.x() + s.radius) / kTileSize)));
    *y0 = std::max(0, static_cast<int>(std::floor((s.mean.y() - s.radius) / kTileSize)));
    *y1 = std::min(grid.rows - 1, static_cast<int>(std::floor((s.mean.y() + s.radius) / kTileSize)));
  };

  for (const ProjectedSplat& splat : projected) {
    int x0, x1, y0, y1;
    tileRange(splat, &x0, &x1, &y0, &y1);
    for (int ty = y0; ty <= y1; ++ty) {
      for (int tx = x0; tx <= x1; ++tx) counts[static_cast<size_t>(ty) * grid.cols + tx + 1]++;
    }
  }
  grid.offsets.assign(counts.begin(), counts.end());
  for (int i = 0; i < tiles; ++i) grid.offsets[i + 1] += grid.offsets[i];
  grid.entries.resize(static_cast<size_t>(grid.offsets[tiles]));

  std::vector<int> cursor(grid.offsets.begin(), grid.offsets.end() - 1);
  for (size_t i = 0; i < projected.size(); ++i) {
    int x0, x1, y0, y1;
    tileRange(projected[i], &x0, &x1, &y0, &y1);
    for (int ty = y0; ty <= y1; ++ty) {
      for (int tx = x0; tx <= x1; ++tx) {
        grid.entries[static_cast<size_t>(cursor[static_cast<size_t>(ty) * grid.cols + tx]++)] =
            static_cast<int>(i);
      }
    }
  }
  return grid;
}

/// Distribute a gradient with respect to the six unique entries of a symmetric
/// matrix across all nine, so the matrix calculus below can treat them as
/// independent.
Eigen::Matrix2f symmetricGradient(const Vec3f& packed) {
  Eigen::Matrix2f m;
  m << packed.x(), 0.5f * packed.y(),
       0.5f * packed.y(), packed.z();
  return m;
}

}  // namespace

void SplatGradients::resize(size_t count, int rest_stride) {
  sh_rest_stride = rest_stride;
  position.assign(count, Vec3f::Zero());
  log_scale.assign(count, Vec3f::Zero());
  rotation.assign(count, Eigen::Vector4f::Zero());
  opacity_logit.assign(count, 0.f);
  dc.assign(count, {0.f, 0.f, 0.f});
  sh_rest.assign(count * static_cast<size_t>(std::max(0, rest_stride)), 0.f);
  screen_gradient.assign(count, 0.f);
  visible_count.assign(count, 0);
}

void SplatGradients::clear() { resize(position.size(), sh_rest_stride); }

std::array<float, 3> evaluateSh(const Gaussian& gaussian, int degree, const Vec3f& direction,
                                std::array<bool, 3>* clamped) {
  std::array<float, 3> color{0.5f + kShC0 * gaussian.dc[0], 0.5f + kShC0 * gaussian.dc[1],
                             0.5f + kShC0 * gaussian.dc[2]};
  float basis[15];
  const int count = shBasis(degree, direction, basis);
  const int stride = static_cast<int>(gaussian.sh_rest.size()) / 3;
  const int usable = std::min(count, stride);
  for (int channel = 0; channel < 3; ++channel) {
    const float* rest = gaussian.sh_rest.data() + static_cast<size_t>(channel) * stride;
    float sum = 0.f;
    for (int k = 0; k < usable; ++k) sum += basis[k] * rest[k];
    color[channel] += sum;
  }
  for (int channel = 0; channel < 3; ++channel) {
    const bool negative = color[channel] < 0.f;
    if (clamped != nullptr) (*clamped)[channel] = negative;
    if (negative) color[channel] = 0.f;
  }
  return color;
}

std::vector<ProjectedSplat> projectSplats(const GaussianCloud& cloud, const SplatView& view,
                                          const RasterOptions& options) {
  std::vector<ProjectedSplat> projected;
  projected.reserve(cloud.size());

  const CameraIntrinsics& k = view.intrinsics;
  const Eigen::Matrix3f W = view.camera_from_world.R.cast<float>();
  const Vec3f t = view.camera_from_world.t.cast<float>();
  const Vec3f camera_center = view.camera_from_world.center().cast<float>();
  const float fx = static_cast<float>(k.fx), fy = static_cast<float>(k.fy);
  const float tan_half_x = static_cast<float>(0.5 * k.width / k.fx);
  const float tan_half_y = static_cast<float>(0.5 * k.height / k.fy);

  for (size_t i = 0; i < cloud.gaussians.size(); ++i) {
    const Gaussian& gaussian = cloud.gaussians[i];
    const Vec3f p_cam = W * gaussian.position + t;
    if (p_cam.z() < 0.01f) continue;

    const Eigen::Matrix<float, 2, 3> J = projectionJacobian(p_cam, fx, fy, tan_half_x, tan_half_y);
    const Eigen::Matrix<float, 2, 3> T = J * W;
    Eigen::Matrix2f cov = T * covariance3d(gaussian) * T.transpose();
    cov(0, 0) += options.screen_dilation;
    cov(1, 1) += options.screen_dilation;

    const float det = cov(0, 0) * cov(1, 1) - cov(0, 1) * cov(1, 0);
    if (det <= 1e-12f) continue;

    ProjectedSplat splat;
    splat.index = static_cast<int>(i);
    splat.mean = Vec2f(fx * p_cam.x() / p_cam.z() + static_cast<float>(k.cx),
                       fy * p_cam.y() / p_cam.z() + static_cast<float>(k.cy));
    splat.cov2d = Vec3f(cov(0, 0), cov(0, 1), cov(1, 1));
    splat.conic = Vec3f(cov(1, 1) / det, -cov(0, 1) / det, cov(0, 0) / det);
    splat.depth = p_cam.z();

    // Three standard deviations of the major axis covers 99.7% of the mass;
    // beyond it the exponential is below the eight-bit quantisation anyway.
    // The larger eigenvalue of a symmetric 2x2 is mid + sqrt(mid^2 - det),
    // and the clamp only guards the square root against rounding noise -- it
    // must not be raised to a comfortable-looking constant, because that
    // inflates every near-isotropic splat and drags in tiles it cannot reach.
    const float mid = 0.5f * (cov(0, 0) + cov(1, 1));
    const float spread = std::sqrt(std::max(0.f, mid * mid - det));
    splat.radius = 3.f * std::sqrt(std::max(mid + spread, 1e-6f));

    if (splat.mean.x() + splat.radius < 0.f || splat.mean.x() - splat.radius >= k.width ||
        splat.mean.y() + splat.radius < 0.f || splat.mean.y() - splat.radius >= k.height) {
      continue;
    }

    splat.opacity = gaussianOpacity(gaussian);
    if (splat.opacity < options.min_alpha) continue;

    const Vec3f direction = (gaussian.position - camera_center).normalized();
    splat.color = evaluateSh(gaussian, std::min(options.sh_degree, cloud.sh_degree), direction,
                             &splat.color_clamped);
    projected.push_back(splat);
  }

  std::sort(projected.begin(), projected.end(),
            [](const ProjectedSplat& a, const ProjectedSplat& b) { return a.depth < b.depth; });
  return projected;
}

SplatImage rasterizeSplats(const std::vector<ProjectedSplat>& projected, const SplatView& view,
                           const RasterOptions& options) {
  SplatImage image;
  image.width = view.intrinsics.width;
  image.height = view.intrinsics.height;
  const size_t pixels = static_cast<size_t>(image.width) * image.height;
  image.rgb.assign(pixels * 3, 0.f);
  image.transmittance.assign(pixels, 1.f);
  if (pixels == 0) return image;

  const TileGrid grid = buildTiles(projected, image.width, image.height);

  for (int ty = 0; ty < grid.rows; ++ty) {
    for (int tx = 0; tx < grid.cols; ++tx) {
      const int tile = ty * grid.cols + tx;
      const int begin = grid.offsets[tile];
      const int end = grid.offsets[tile + 1];
      const int y_end = std::min(image.height, (ty + 1) * kTileSize);
      const int x_end = std::min(image.width, (tx + 1) * kTileSize);

      for (int y = ty * kTileSize; y < y_end; ++y) {
        for (int x = tx * kTileSize; x < x_end; ++x) {
          const size_t pixel = static_cast<size_t>(y) * image.width + x;
          const Vec2f point(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
          float transmittance = 1.f;
          float accumulated[3] = {0.f, 0.f, 0.f};

          for (int e = begin; e < end; ++e) {
            const ProjectedSplat& splat = projected[static_cast<size_t>(grid.entries[e])];
            const Vec2f d = point - splat.mean;
            const float power = -0.5f * (splat.conic.x() * d.x() * d.x() +
                                         splat.conic.z() * d.y() * d.y()) -
                                splat.conic.y() * d.x() * d.y();
            if (power > 0.f) continue;
            const float alpha = std::min(kMaxAlpha, splat.opacity * std::exp(power));
            if (alpha < options.min_alpha) continue;
            for (int c = 0; c < 3; ++c) accumulated[c] += transmittance * alpha * splat.color[c];
            transmittance *= 1.f - alpha;
            if (transmittance < options.transmittance_cutoff) break;
          }

          for (int c = 0; c < 3; ++c) {
            image.rgb[pixel * 3 + c] = accumulated[c] + transmittance * options.background[c];
          }
          image.transmittance[pixel] = transmittance;
        }
      }
    }
  }
  return image;
}

SplatImage renderSplats(const GaussianCloud& cloud, const SplatView& view,
                        const RasterOptions& options) {
  return rasterizeSplats(projectSplats(cloud, view, options), view, options);
}

void rasterizeSplatsBackward(const GaussianCloud& cloud, const SplatView& view,
                             const RasterOptions& options,
                             const std::vector<ProjectedSplat>& projected,
                             const SplatImage& forward, const std::vector<float>& dL_drgb,
                             SplatGradients* out) {
  if (out->position.size() != cloud.size()) {
    out->resize(cloud.size(),
                cloud.gaussians.empty()
                    ? 0
                    : static_cast<int>(cloud.gaussians.front().sh_rest.size()));
  }
  if (projected.empty() || forward.width == 0) return;

  // Per-splat screen-space accumulators, indexed by position in `projected`.
  const size_t count = projected.size();
  std::vector<Vec2f> dL_dmean(count, Vec2f::Zero());
  std::vector<Vec3f> dL_dconic(count, Vec3f::Zero());
  std::vector<float> dL_dopacity(count, 0.f);
  std::vector<std::array<float, 3>> dL_dcolor(count, {0.f, 0.f, 0.f});
  std::vector<float> dL_dmean_abs(count, 0.f);
  std::vector<int> touched(count, 0);

  const TileGrid grid = buildTiles(projected, forward.width, forward.height);

  for (int ty = 0; ty < grid.rows; ++ty) {
    for (int tx = 0; tx < grid.cols; ++tx) {
      const int tile = ty * grid.cols + tx;
      const int begin = grid.offsets[tile];
      const int end = grid.offsets[tile + 1];
      const int y_end = std::min(forward.height, (ty + 1) * kTileSize);
      const int x_end = std::min(forward.width, (tx + 1) * kTileSize);

      for (int y = ty * kTileSize; y < y_end; ++y) {
        for (int x = tx * kTileSize; x < x_end; ++x) {
          const size_t pixel = static_cast<size_t>(y) * forward.width + x;
          const Vec2f point(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
          const float* dL_dpixel = &dL_drgb[pixel * 3];

          // The forward pass is replayed to find where it stopped, because the
          // backward walk has to start from the same last contributor.
          float transmittance = 1.f;
          int last = begin;
          for (int e = begin; e < end; ++e) {
            const ProjectedSplat& splat = projected[static_cast<size_t>(grid.entries[e])];
            const Vec2f d = point - splat.mean;
            const float power = -0.5f * (splat.conic.x() * d.x() * d.x() +
                                         splat.conic.z() * d.y() * d.y()) -
                                splat.conic.y() * d.x() * d.y();
            if (power > 0.f) continue;
            const float alpha = std::min(kMaxAlpha, splat.opacity * std::exp(power));
            if (alpha < options.min_alpha) continue;
            transmittance *= 1.f - alpha;
            last = e + 1;
            if (transmittance < options.transmittance_cutoff) break;
          }
          const float final_transmittance = transmittance;

          // Walking back to front lets the colour behind each gaussian be
          // reconstructed incrementally, which is what the alpha gradient needs.
          float suffix[3] = {0.f, 0.f, 0.f};
          float last_alpha = 0.f;
          float last_color[3] = {0.f, 0.f, 0.f};
          float background_term = 0.f;
          for (int c = 0; c < 3; ++c) background_term += options.background[c] * dL_dpixel[c];

          for (int e = last - 1; e >= begin; --e) {
            const size_t s = static_cast<size_t>(grid.entries[e]);
            const ProjectedSplat& splat = projected[s];
            const Vec2f d = point - splat.mean;
            const float power = -0.5f * (splat.conic.x() * d.x() * d.x() +
                                         splat.conic.z() * d.y() * d.y()) -
                                splat.conic.y() * d.x() * d.y();
            if (power > 0.f) continue;
            const float gaussian_weight = std::exp(power);
            const float raw_alpha = splat.opacity * gaussian_weight;
            const float alpha = std::min(kMaxAlpha, raw_alpha);
            if (alpha < options.min_alpha) continue;

            transmittance /= 1.f - alpha;
            const float weight = alpha * transmittance;

            float dL_dalpha = 0.f;
            for (int c = 0; c < 3; ++c) {
              dL_dcolor[s][c] += weight * dL_dpixel[c];
              suffix[c] = last_alpha * last_color[c] + (1.f - last_alpha) * suffix[c];
              last_color[c] = splat.color[c];
              dL_dalpha += (splat.color[c] - suffix[c]) * transmittance * dL_dpixel[c];
            }
            last_alpha = alpha;
            // Making this gaussian more opaque removes background from the pixel.
            dL_dalpha += -final_transmittance / (1.f - alpha) * background_term;

            if (raw_alpha >= kMaxAlpha) continue;  // the cap flattens the gradient

            dL_dopacity[s] += gaussian_weight * dL_dalpha;
            const float dL_dweight = splat.opacity * dL_dalpha;
            const float gx = gaussian_weight * d.x();
            const float gy = gaussian_weight * d.y();
            const Vec2f dmean(dL_dweight * (-gx * splat.conic.x() - gy * splat.conic.y()),
                              dL_dweight * (-gy * splat.conic.z() - gx * splat.conic.y()));
            // d(mean) = -d(delta), and the pixel is fixed, so the sign flips.
            dL_dmean[s] -= dmean;
            dL_dmean_abs[s] += dmean.norm();
            dL_dconic[s] += Vec3f(-0.5f * dL_dweight * gx * d.x(),
                                  -1.0f * dL_dweight * gx * d.y(),
                                  -0.5f * dL_dweight * gy * d.y());
            touched[s] = 1;
          }
        }
      }
    }
  }

  // ---- screen space back to the gaussian's own parameters -----------------

  const CameraIntrinsics& k = view.intrinsics;
  const Eigen::Matrix3f W = view.camera_from_world.R.cast<float>();
  const Vec3f t = view.camera_from_world.t.cast<float>();
  const Vec3f camera_center = view.camera_from_world.center().cast<float>();
  const float fx = static_cast<float>(k.fx), fy = static_cast<float>(k.fy);
  const float tan_half_x = static_cast<float>(0.5 * k.width / k.fx);
  const float tan_half_y = static_cast<float>(0.5 * k.height / k.fy);
  const int rest_stride = out->sh_rest_stride;

  for (size_t s = 0; s < count; ++s) {
    const ProjectedSplat& splat = projected[s];
    const size_t g = static_cast<size_t>(splat.index);
    const Gaussian& gaussian = cloud.gaussians[g];

    out->visible_count[g] += 1;
    out->screen_gradient[g] += dL_dmean_abs[s];
    if (touched[s] == 0) continue;

    // opacity: alpha = sigmoid(logit) * G
    const float opacity = splat.opacity;
    out->opacity_logit[g] += dL_dopacity[s] * opacity * (1.f - opacity);

    // colour: through the SH clamp, into the coefficients and the direction
    std::array<float, 3> dL_dcol = dL_dcolor[s];
    for (int c = 0; c < 3; ++c) {
      if (splat.color_clamped[c]) dL_dcol[c] = 0.f;
      out->dc[g][c] += kShC0 * dL_dcol[c];
    }

    Vec3f dL_ddirection = Vec3f::Zero();
    const int degree = std::min(options.sh_degree, cloud.sh_degree);
    const int per_channel = rest_stride / 3;
    const Vec3f offset = gaussian.position - camera_center;
    const float distance = offset.norm();
    if (degree >= 1 && distance > 1e-9f && per_channel > 0) {
      const Vec3f direction = offset / distance;
      float basis[15];
      const int basis_count = shBasis(degree, direction, basis);
      const int usable = std::min(basis_count, per_channel);
      Vec3f jacobian[15];
      shBasisJacobian(degree, direction, basis_count, jacobian);
      for (int c = 0; c < 3; ++c) {
        if (dL_dcol[c] == 0.f) continue;
        const size_t channel_offset = static_cast<size_t>(c) * per_channel;
        const float* rest = gaussian.sh_rest.data() + channel_offset;
        float* destination = out->sh_rest.data() + g * rest_stride + channel_offset;
        for (int b = 0; b < usable; ++b) {
          destination[b] += basis[b] * dL_dcol[c];
          dL_ddirection += jacobian[b] * (rest[b] * dL_dcol[c]);
        }
      }
    }

    // mean: pixels = f * x / z + c
    const Vec3f p_cam = W * gaussian.position + t;
    const float z = p_cam.z();
    Vec3f dL_dp_cam(fx / z * dL_dmean[s].x(), fy / z * dL_dmean[s].y(),
                    -(fx * p_cam.x() * dL_dmean[s].x() + fy * p_cam.y() * dL_dmean[s].y()) /
                        (z * z));

    // conic -> 2D covariance: d(A^-1) = -A^-1 dA A^-1
    Eigen::Matrix2f conic_matrix;
    conic_matrix << splat.conic.x(), splat.conic.y(), splat.conic.y(), splat.conic.z();
    const Eigen::Matrix2f dL_dconic_matrix = symmetricGradient(dL_dconic[s]);
    const Eigen::Matrix2f dL_dcov2d_matrix =
        -(conic_matrix * dL_dconic_matrix * conic_matrix);

    // 2D covariance -> 3D covariance and the projection Jacobian
    const Eigen::Matrix<float, 2, 3> J = projectionJacobian(p_cam, fx, fy, tan_half_x, tan_half_y);
    const Eigen::Matrix<float, 2, 3> T = J * W;
    const Eigen::Matrix3f cov3d = covariance3d(gaussian);
    const Eigen::Matrix3f dL_dcov3d = T.transpose() * dL_dcov2d_matrix * T;
    const Eigen::Matrix<float, 2, 3> dL_dT = 2.f * dL_dcov2d_matrix * T * cov3d;
    const Eigen::Matrix<float, 2, 3> dL_dJ = dL_dT * W.transpose();

    const bool clamped_x =
        std::abs(p_cam.x() / z) > tan_half_x * kFrustumSlack;
    const bool clamped_y =
        std::abs(p_cam.y() / z) > tan_half_y * kFrustumSlack;
    const float jx = clamped_x ? std::copysign(tan_half_x * kFrustumSlack, p_cam.x()) * z
                               : p_cam.x();
    const float jy = clamped_y ? std::copysign(tan_half_y * kFrustumSlack, p_cam.y()) * z
                               : p_cam.y();
    if (!clamped_x) dL_dp_cam.x() += dL_dJ(0, 2) * (-fx / (z * z));
    if (!clamped_y) dL_dp_cam.y() += dL_dJ(1, 2) * (-fy / (z * z));
    // Once a coordinate is clamped the Jacobian entry is -f * limit / z, whose
    // z-derivative is half what the unclamped -f * x / z^2 gives.
    const float dz_x = (clamped_x ? 1.f : 2.f) * fx * jx / (z * z * z);
    const float dz_y = (clamped_y ? 1.f : 2.f) * fy * jy / (z * z * z);
    dL_dp_cam.z() += dL_dJ(0, 0) * (-fx / (z * z)) + dL_dJ(1, 1) * (-fy / (z * z)) +
                     dL_dJ(0, 2) * dz_x + dL_dJ(1, 2) * dz_y;

    Vec3f dL_dposition = W.transpose() * dL_dp_cam;

    // the view direction the SH were evaluated along also moves with position
    if (distance > 1e-9f && dL_ddirection.squaredNorm() > 0.f) {
      const Vec3f direction = offset / distance;
      dL_dposition += (dL_ddirection - direction * direction.dot(dL_ddirection)) / distance;
    }
    out->position[g] += dL_dposition;

    // 3D covariance -> scale and rotation, through Sigma = (R S)(R S)^T
    const Eigen::Matrix3f R = rotationOf(gaussian);
    const Vec3f scale = gaussian.log_scale.array().exp();
    const Eigen::Matrix3f M = R * scale.asDiagonal();
    const Eigen::Matrix3f symmetric = 0.5f * (dL_dcov3d + dL_dcov3d.transpose());
    const Eigen::Matrix3f dL_dM = 2.f * symmetric * M;

    const Eigen::Matrix3f dL_dS = R.transpose() * dL_dM;
    for (int a = 0; a < 3; ++a) out->log_scale[g][a] += dL_dS(a, a) * scale[a];

    Eigen::Matrix3f dL_dR;
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) dL_dR(row, col) = dL_dM(row, col) * scale[col];
    }
    // The forward pass normalises the quaternion, so the gradient is first
    // taken with respect to the unit one and then pushed back through the
    // normalisation; without that step the component along the quaternion
    // itself is wrong, which is invisible for most rotations and badly wrong
    // for some.
    const float raw_norm = std::max(gaussian.rotation.norm(), 1e-12f);
    const Eigen::Quaternionf q = gaussian.rotation.normalized();
    const float qw = q.w(), qx = q.x(), qy = q.y(), qz = q.z();
    Eigen::Vector4f dL_dq = Eigen::Vector4f::Zero();  // (w, x, y, z)
    dL_dq[0] += 2.f * (-qz * dL_dR(0, 1) + qy * dL_dR(0, 2) + qz * dL_dR(1, 0) -
                       qx * dL_dR(1, 2) - qy * dL_dR(2, 0) + qx * dL_dR(2, 1));
    dL_dq[1] += 2.f * (qy * dL_dR(0, 1) + qz * dL_dR(0, 2) + qy * dL_dR(1, 0) -
                       2.f * qx * dL_dR(1, 1) - qw * dL_dR(1, 2) + qz * dL_dR(2, 0) +
                       qw * dL_dR(2, 1) - 2.f * qx * dL_dR(2, 2));
    dL_dq[2] += 2.f * (-2.f * qy * dL_dR(0, 0) + qx * dL_dR(0, 1) + qw * dL_dR(0, 2) +
                       qx * dL_dR(1, 0) + qz * dL_dR(1, 2) - qw * dL_dR(2, 0) +
                       qz * dL_dR(2, 1) - 2.f * qy * dL_dR(2, 2));
    dL_dq[3] += 2.f * (-2.f * qz * dL_dR(0, 0) - qw * dL_dR(0, 1) + qx * dL_dR(0, 2) +
                       qw * dL_dR(1, 0) - 2.f * qz * dL_dR(1, 1) + qy * dL_dR(1, 2) +
                       qx * dL_dR(2, 0) + qy * dL_dR(2, 1));

    const Eigen::Vector4f unit(qw, qx, qy, qz);
    out->rotation[g] += (dL_dq - unit * unit.dot(dL_dq)) / raw_norm;
  }
}

// --------------------------------------------------------------- the loss ---

namespace {

constexpr int kSsimRadius = 5;  // an 11-tap window
constexpr float kSsimSigma = 1.5f;
constexpr float kSsimC1 = 0.01f * 0.01f;
constexpr float kSsimC2 = 0.03f * 0.03f;

const std::vector<float>& ssimKernel() {
  static const std::vector<float> kernel = [] {
    std::vector<float> values(2 * kSsimRadius + 1);
    float total = 0.f;
    for (int i = -kSsimRadius; i <= kSsimRadius; ++i) {
      const float v = std::exp(-0.5f * static_cast<float>(i * i) / (kSsimSigma * kSsimSigma));
      values[static_cast<size_t>(i + kSsimRadius)] = v;
      total += v;
    }
    for (float& v : values) v /= total;
    return values;
  }();
  return kernel;
}

/**
 * Separable blur with zero padding.
 *
 * Zero padding rather than edge clamping, because the gradient below applies
 * the same blur a second time as the adjoint of this one, and that identity
 * only holds for a symmetric kernel over a zero-padded domain. Clamping folds
 * the out-of-bounds weight back onto the border pixels, whose adjoint is a
 * different operator -- which showed up as a 40% gradient error in the corner
 * and nowhere else. The cost is that similarity within half a window of the
 * edge is measured against a darkened mean, equally for both images; the
 * reference implementation makes the same trade.
 */
void blur(const std::vector<float>& src, int width, int height, std::vector<float>* dst) {
  const std::vector<float>& kernel = ssimKernel();
  std::vector<float> horizontal(src.size(), 0.f);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum = 0.f;
      for (int i = -kSsimRadius; i <= kSsimRadius; ++i) {
        const int sx = x + i;
        if (sx < 0 || sx >= width) continue;
        sum += kernel[static_cast<size_t>(i + kSsimRadius)] *
               src[static_cast<size_t>(y) * width + sx];
      }
      horizontal[static_cast<size_t>(y) * width + x] = sum;
    }
  }
  dst->assign(src.size(), 0.f);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum = 0.f;
      for (int i = -kSsimRadius; i <= kSsimRadius; ++i) {
        const int sy = y + i;
        if (sy < 0 || sy >= height) continue;
        sum += kernel[static_cast<size_t>(i + kSsimRadius)] *
               horizontal[static_cast<size_t>(sy) * width + x];
      }
      (*dst)[static_cast<size_t>(y) * width + x] = sum;
    }
  }
}

}  // namespace

float meanSsim(const std::vector<float>& rendered, const std::vector<float>& target, int width,
               int height, std::vector<float>* d_rendered) {
  const size_t pixels = static_cast<size_t>(width) * height;
  if (pixels == 0 || rendered.size() != pixels * 3 || target.size() != pixels * 3) return 0.f;
  if (d_rendered != nullptr) d_rendered->assign(pixels * 3, 0.f);

  const float scale = 1.f / static_cast<float>(pixels * 3);
  float total = 0.f;

  std::vector<float> x(pixels), y(pixels), xx(pixels), yy(pixels), xy(pixels);
  std::vector<float> mu_x, mu_y, exx, eyy, exy;
  std::vector<float> term_mu(pixels), term_var(pixels), term_cov(pixels);
  std::vector<float> blurred;

  for (int channel = 0; channel < 3; ++channel) {
    for (size_t p = 0; p < pixels; ++p) {
      x[p] = rendered[p * 3 + channel];
      y[p] = target[p * 3 + channel];
      xx[p] = x[p] * x[p];
      yy[p] = y[p] * y[p];
      xy[p] = x[p] * y[p];
    }
    blur(x, width, height, &mu_x);
    blur(y, width, height, &mu_y);
    blur(xx, width, height, &exx);
    blur(yy, width, height, &eyy);
    blur(xy, width, height, &exy);

    for (size_t p = 0; p < pixels; ++p) {
      const float var_x = exx[p] - mu_x[p] * mu_x[p];
      const float var_y = eyy[p] - mu_y[p] * mu_y[p];
      const float cov = exy[p] - mu_x[p] * mu_y[p];
      const float a = 2.f * mu_x[p] * mu_y[p] + kSsimC1;
      const float b = 2.f * cov + kSsimC2;
      const float c = mu_x[p] * mu_x[p] + mu_y[p] * mu_y[p] + kSsimC1;
      const float d = var_x + var_y + kSsimC2;
      total += (a * b) / (c * d);
      if (d_rendered == nullptr) continue;
      // Partials of the SSIM map with respect to this window's statistics.
      term_mu[p] = scale * (2.f * mu_y[p] * b / (c * d) - 2.f * mu_x[p] * a * b / (c * c * d));
      term_var[p] = scale * (-(a * b) / (c * d * d));
      term_cov[p] = scale * (2.f * a / (c * d));
    }
    if (d_rendered == nullptr) continue;

    // Each statistic is itself a blur, and the blur kernel is symmetric, so
    // the chain rule is another blur of the per-window partials.
    std::vector<float> scratch(pixels);
    blur(term_mu, width, height, &blurred);
    for (size_t p = 0; p < pixels; ++p) (*d_rendered)[p * 3 + channel] += blurred[p];

    blur(term_var, width, height, &blurred);
    for (size_t p = 0; p < pixels; ++p) (*d_rendered)[p * 3 + channel] += 2.f * x[p] * blurred[p];
    for (size_t p = 0; p < pixels; ++p) scratch[p] = term_var[p] * mu_x[p];
    blur(scratch, width, height, &blurred);
    for (size_t p = 0; p < pixels; ++p) (*d_rendered)[p * 3 + channel] -= 2.f * blurred[p];

    blur(term_cov, width, height, &blurred);
    for (size_t p = 0; p < pixels; ++p) (*d_rendered)[p * 3 + channel] += y[p] * blurred[p];
    for (size_t p = 0; p < pixels; ++p) scratch[p] = term_cov[p] * mu_y[p];
    blur(scratch, width, height, &blurred);
    for (size_t p = 0; p < pixels; ++p) (*d_rendered)[p * 3 + channel] -= blurred[p];
  }
  return total * scale;
}

float photometricLoss(const SplatImage& rendered, const std::vector<float>& target,
                      float ssim_weight, std::vector<float>* dL_drgb) {
  const size_t values = rendered.rgb.size();
  if (dL_drgb != nullptr) dL_drgb->assign(values, 0.f);
  if (values == 0 || target.size() != values) return 0.f;

  const float inverse = 1.f / static_cast<float>(values);
  float l1 = 0.f;
  for (size_t i = 0; i < values; ++i) {
    const float difference = rendered.rgb[i] - target[i];
    l1 += std::abs(difference);
    if (dL_drgb != nullptr) {
      const float sign = difference > 0.f ? 1.f : (difference < 0.f ? -1.f : 0.f);
      (*dL_drgb)[i] = (1.f - ssim_weight) * sign * inverse;
    }
  }
  l1 *= inverse;
  if (ssim_weight <= 0.f) return l1;

  std::vector<float> d_ssim;
  const float ssim = meanSsim(rendered.rgb, target, rendered.width, rendered.height,
                              dL_drgb != nullptr ? &d_ssim : nullptr);
  if (dL_drgb != nullptr) {
    for (size_t i = 0; i < values; ++i) (*dL_drgb)[i] -= ssim_weight * d_ssim[i];
  }
  return (1.f - ssim_weight) * l1 + ssim_weight * (1.f - ssim);
}

}  // namespace parallax
