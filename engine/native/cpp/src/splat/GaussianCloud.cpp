#include "parallax/splat/GaussianCloud.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "parallax/io/ImageIo.h"

namespace parallax {
namespace {

template <typename T>
void appendRaw(std::vector<uint8_t>* out, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool takeRaw(const std::vector<uint8_t>& in, size_t* pos, T* value) {
  if (*pos + sizeof(T) > in.size()) return false;
  std::memcpy(value, in.data() + *pos, sizeof(T));
  *pos += sizeof(T);
  return true;
}

uint8_t quantizeUnit(float v) {
  return static_cast<uint8_t>(std::max(0.f, std::min(255.f, std::round(v * 255.f))));
}

/// gzip-wrapped deflate, which is what .spz uses.
bool gzipCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream stream{};
  // 16 added to windowBits selects a gzip wrapper rather than zlib's own.
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  out->resize(deflateBound(&stream, static_cast<uLong>(in.size())) + 64);
  stream.next_in = const_cast<Bytef*>(in.data());
  stream.avail_in = static_cast<uInt>(in.size());
  stream.next_out = out->data();
  stream.avail_out = static_cast<uInt>(out->size());
  const int rc = deflate(&stream, Z_FINISH);
  const size_t written = out->size() - stream.avail_out;
  deflateEnd(&stream);
  if (rc != Z_STREAM_END) return false;
  out->resize(written);
  return true;
}

bool gzipDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream stream{};
  // 32 added to windowBits accepts either a gzip or a zlib wrapper.
  if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(in.data());
  stream.avail_in = static_cast<uInt>(in.size());

  out->clear();
  std::vector<uint8_t> chunk(1 << 16);
  int rc = Z_OK;
  // Bound the output so a crafted file cannot exhaust memory.
  constexpr size_t kMaxOutput = 1ull << 31;
  do {
    stream.next_out = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&stream, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) break;
    out->insert(out->end(), chunk.data(), chunk.data() + (chunk.size() - stream.avail_out));
    if (out->size() > kMaxOutput) {
      inflateEnd(&stream);
      return false;
    }
  } while (rc != Z_STREAM_END);
  inflateEnd(&stream);
  return rc == Z_STREAM_END;
}

// ---- .spz quantisation -----------------------------------------------------
//
// These constants come from the published behaviour of the reference encoder.
// They are gathered here, named, so that a future correction is a one-line
// change rather than a hunt through the writer and the reader.

/// Scales are stored as log(sigma) mapped onto a byte. The offset of 10 and the
/// gain of 16 cover log-scales in roughly [-10, +6], which spans every splat a
/// real scene produces.
constexpr float kSpzScaleOffset = 10.0f;
constexpr float kSpzScaleGain = 16.0f;

uint8_t spzEncodeScale(float log_scale) {
  return static_cast<uint8_t>(
      std::max(0.f, std::min(255.f, std::round((log_scale + kSpzScaleOffset) * kSpzScaleGain))));
}
float spzDecodeScale(uint8_t byte) {
  return static_cast<float>(byte) / kSpzScaleGain - kSpzScaleOffset;
}

/// Quaternion x, y, z in [-1, 1] mapped onto a byte; w is recovered from the
/// unit-norm constraint on read.
uint8_t spzEncodeQuatComponent(float v) {
  return static_cast<uint8_t>(
      std::max(0.f, std::min(255.f, std::round(v * 127.5f + 127.5f))));
}
float spzDecodeQuatComponent(uint8_t byte) { return (static_cast<float>(byte) - 127.5f) / 127.5f; }

/// Colour is the DC coefficient mapped through the same transform a viewer
/// applies, so the stored byte is a linear RGB value.
uint8_t spzEncodeColor(float dc) { return quantizeUnit(0.5f + kShC0 * dc); }
float spzDecodeColor(uint8_t byte) { return (static_cast<float>(byte) / 255.f - 0.5f) / kShC0; }

/// Higher-order SH coefficients, centred on 128.
uint8_t spzEncodeSh(float v) {
  return static_cast<uint8_t>(
      std::max(0.f, std::min(255.f, std::round(v * 128.0f + 128.0f))));
}
float spzDecodeSh(uint8_t byte) { return (static_cast<float>(byte) - 128.0f) / 128.0f; }

/// 24-bit signed fixed point, little-endian.
void spzEncodePosition(float value, int fractional_bits, std::vector<uint8_t>* out) {
  const float scale = static_cast<float>(1 << fractional_bits);
  int32_t fixed = static_cast<int32_t>(std::lround(value * scale));
  // Clamp into the 24-bit signed range rather than letting it wrap.
  fixed = std::max(-(1 << 23), std::min((1 << 23) - 1, fixed));
  const uint32_t bits = static_cast<uint32_t>(fixed) & 0xFFFFFFu;
  out->push_back(static_cast<uint8_t>(bits & 0xFF));
  out->push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
}

float spzDecodePosition(const uint8_t* p, int fractional_bits) {
  uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16);
  // Sign-extend from 24 bits.
  if (bits & 0x800000u) bits |= 0xFF000000u;
  const int32_t fixed = static_cast<int32_t>(bits);
  return static_cast<float>(fixed) / static_cast<float>(1 << fractional_bits);
}

constexpr uint32_t kSpzMagic = 0x5053474eu;  // "NGSP"
constexpr uint32_t kSpzVersion = 2u;

}  // namespace

bool GaussianCloud::isValid(std::string* reason) const {
  const auto fail = [reason](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (sh_degree < 0 || sh_degree > 3) return fail("sh_degree must be 0..3");
  const size_t expected_rest = static_cast<size_t>(shRestPerChannel(sh_degree)) * 3;
  for (const Gaussian& g : gaussians) {
    if (g.sh_rest.size() != expected_rest) {
      return fail("sh_rest length does not match sh_degree");
    }
    if (!std::isfinite(g.position.x()) || !std::isfinite(g.position.y()) ||
        !std::isfinite(g.position.z())) {
      return fail("non-finite gaussian position");
    }
    if (!std::isfinite(g.opacity_logit)) return fail("non-finite opacity");
    const float norm = g.rotation.norm();
    if (!std::isfinite(norm) || norm < 1e-3f) return fail("degenerate rotation quaternion");
  }
  if (reason) reason->clear();
  return true;
}

AxisAlignedBox GaussianCloud::bounds() const {
  AxisAlignedBox box;
  for (const Gaussian& g : gaussians) {
    box.expand(Vec3(g.position.x(), g.position.y(), g.position.z()));
  }
  return box;
}

int GaussianCloud::prune(float min_opacity, float max_scale) {
  const size_t before = gaussians.size();
  gaussians.erase(
      std::remove_if(gaussians.begin(), gaussians.end(),
                     [min_opacity, max_scale](const Gaussian& g) {
                       if (gaussianOpacity(g) < min_opacity) return true;
                       // A splat far larger than the scene is a training
                       // artefact that smears colour across everything.
                       const float largest = std::exp(g.log_scale.maxCoeff());
                       return !std::isfinite(largest) || largest > max_scale;
                     }),
      gaussians.end());
  return static_cast<int>(before - gaussians.size());
}

void GaussianCloud::sortByOpacity() {
  std::stable_sort(gaussians.begin(), gaussians.end(),
                   [](const Gaussian& a, const Gaussian& b) {
                     return a.opacity_logit > b.opacity_logit;
                   });
}

GaussianCloud initializeFromPointCloud(const std::vector<Vec3f>& points,
                                       const std::vector<std::array<uint8_t, 3>>& colors,
                                       int sh_degree, float initial_opacity) {
  GaussianCloud cloud;
  cloud.sh_degree = std::max(0, std::min(3, sh_degree));
  if (points.empty()) return cloud;

  // Estimate a representative point spacing from a sample of nearest-neighbour
  // distances. Initialising every splat at the mean spacing makes the starting
  // field just dense enough to cover the surface without a huge opaque blob,
  // which is what the reference implementation's knn initialisation achieves.
  double spacing = 0.0;
  {
    const size_t sample = std::min<size_t>(points.size(), 512);
    const size_t stride = std::max<size_t>(1, points.size() / sample);
    int counted = 0;
    for (size_t i = 0; i < points.size(); i += stride) {
      double nearest = 1e30;
      for (size_t j = 0; j < points.size(); j += stride) {
        if (i == j) continue;
        nearest = std::min(nearest, static_cast<double>((points[i] - points[j]).norm()));
      }
      if (nearest < 1e29) {
        spacing += nearest;
        ++counted;
      }
    }
    spacing = counted > 0 ? spacing / counted : 0.01;
  }
  // A single sampled stride over-estimates the true spacing; scale it back.
  const double sigma = std::max(1e-4, spacing * 0.5);
  const float log_sigma = static_cast<float>(std::log(sigma));

  const size_t rest = static_cast<size_t>(shRestPerChannel(cloud.sh_degree)) * 3;
  const bool has_colors = colors.size() == points.size();
  const float logit =
      std::log(std::max(1e-6f, initial_opacity) / std::max(1e-6f, 1.f - initial_opacity));

  cloud.gaussians.reserve(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    Gaussian g;
    g.position = points[i];
    g.log_scale = Vec3f::Constant(log_sigma);
    g.rotation = Eigen::Quaternionf::Identity();
    g.opacity_logit = logit;
    if (has_colors) {
      // Invert the viewer's DC-to-colour transform so the splat starts at the
      // observed colour.
      for (int c = 0; c < 3; ++c) {
        g.dc[c] = (colors[i][c] / 255.0f - 0.5f) / kShC0;
      }
    }
    g.sh_rest.assign(rest, 0.f);
    cloud.gaussians.push_back(std::move(g));
  }
  return cloud;
}

// --------------------------------------------------------------- 3DGS PLY ---

bool writeGaussianPly(const std::string& path, const GaussianCloud& cloud) {
  if (cloud.empty()) return false;
  const int rest_per_channel = shRestPerChannel(cloud.sh_degree);
  const int rest_total = rest_per_channel * 3;

  std::ostringstream header;
  header << "ply\nformat binary_little_endian 1.0\n";
  header << "comment generated by Parallax\n";
  header << "element vertex " << cloud.size() << "\n";
  header << "property float x\nproperty float y\nproperty float z\n";
  // Normals are not used by 3DGS but the reference format includes them, and
  // several viewers index properties positionally rather than by name.
  header << "property float nx\nproperty float ny\nproperty float nz\n";
  for (int i = 0; i < 3; ++i) header << "property float f_dc_" << i << "\n";
  for (int i = 0; i < rest_total; ++i) header << "property float f_rest_" << i << "\n";
  header << "property float opacity\n";
  for (int i = 0; i < 3; ++i) header << "property float scale_" << i << "\n";
  for (int i = 0; i < 4; ++i) header << "property float rot_" << i << "\n";
  header << "end_header\n";

  std::vector<uint8_t> bytes;
  const std::string header_text = header.str();
  bytes.insert(bytes.end(), header_text.begin(), header_text.end());
  bytes.reserve(bytes.size() + cloud.size() * (17 + rest_total) * sizeof(float));

  for (const Gaussian& g : cloud.gaussians) {
    appendRaw(&bytes, g.position.x());
    appendRaw(&bytes, g.position.y());
    appendRaw(&bytes, g.position.z());
    appendRaw(&bytes, 0.f);
    appendRaw(&bytes, 0.f);
    appendRaw(&bytes, 0.f);
    for (int c = 0; c < 3; ++c) appendRaw(&bytes, g.dc[c]);
    for (int i = 0; i < rest_total; ++i) {
      appendRaw(&bytes, i < static_cast<int>(g.sh_rest.size()) ? g.sh_rest[i] : 0.f);
    }
    appendRaw(&bytes, g.opacity_logit);
    appendRaw(&bytes, g.log_scale.x());
    appendRaw(&bytes, g.log_scale.y());
    appendRaw(&bytes, g.log_scale.z());
    // rot_0 is w in the reference implementation's ordering.
    appendRaw(&bytes, g.rotation.w());
    appendRaw(&bytes, g.rotation.x());
    appendRaw(&bytes, g.rotation.y());
    appendRaw(&bytes, g.rotation.z());
  }
  return writeFileBytes(path, bytes);
}

bool readGaussianPly(const std::string& path, GaussianCloud* out, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return fail("cannot read " + path);

  // Parse just enough of the header to learn the property order.
  const std::string text(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), 65536));
  const size_t header_end = text.find("end_header\n");
  if (text.compare(0, 3, "ply") != 0 || header_end == std::string::npos) {
    return fail("not a PLY file");
  }
  if (text.find("binary_little_endian") == std::string::npos) {
    return fail("only binary little-endian 3DGS PLY is supported");
  }

  std::vector<std::string> properties;
  size_t count = 0;
  {
    std::istringstream lines(text.substr(0, header_end));
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream ls(line);
      std::string keyword;
      ls >> keyword;
      if (keyword == "element") {
        std::string name;
        ls >> name >> count;
      } else if (keyword == "property") {
        std::string type, name;
        ls >> type >> name;
        if (type != "float" && type != "float32") {
          return fail("3DGS PLY properties must be float");
        }
        properties.push_back(name);
      }
    }
  }
  if (count == 0 || properties.empty()) return fail("3DGS PLY has no vertices");

  // Locate each property by name, which is what makes this robust to the
  // variations between exporters.
  const auto indexOf = [&properties](const std::string& name) -> int {
    const auto it = std::find(properties.begin(), properties.end(), name);
    return it == properties.end() ? -1 : static_cast<int>(it - properties.begin());
  };
  const int ix = indexOf("x"), iy = indexOf("y"), iz = indexOf("z");
  const int iop = indexOf("opacity");
  if (ix < 0 || iy < 0 || iz < 0 || iop < 0) return fail("3DGS PLY is missing core properties");

  int rest_count = 0;
  while (indexOf("f_rest_" + std::to_string(rest_count)) >= 0) ++rest_count;
  if (rest_count % 3 != 0) return fail("f_rest count is not a multiple of 3");
  const int rest_per_channel = rest_count / 3;
  int degree = 0;
  for (int d = 0; d <= 3; ++d) {
    if (shRestPerChannel(d) == rest_per_channel) degree = d;
  }
  if (shRestPerChannel(degree) != rest_per_channel) {
    return fail("f_rest count does not correspond to an SH degree");
  }

  const size_t stride = properties.size() * sizeof(float);
  const size_t body = header_end + std::strlen("end_header\n");
  if (bytes.size() < body + stride * count) return fail("3DGS PLY body is truncated");

  out->gaussians.clear();
  out->sh_degree = degree;
  out->gaussians.reserve(count);
  const auto value = [&bytes, body, stride](size_t i, int property) {
    float v = 0.f;
    std::memcpy(&v, bytes.data() + body + i * stride + property * sizeof(float), sizeof(float));
    return v;
  };

  for (size_t i = 0; i < count; ++i) {
    Gaussian g;
    g.position = Vec3f(value(i, ix), value(i, iy), value(i, iz));
    g.opacity_logit = value(i, iop);
    for (int c = 0; c < 3; ++c) {
      const int index = indexOf("f_dc_" + std::to_string(c));
      g.dc[c] = index >= 0 ? value(i, index) : 0.f;
    }
    for (int c = 0; c < 3; ++c) {
      const int index = indexOf("scale_" + std::to_string(c));
      g.log_scale[c] = index >= 0 ? value(i, index) : 0.f;
    }
    const int rw = indexOf("rot_0"), rx = indexOf("rot_1"), ry = indexOf("rot_2"),
              rz = indexOf("rot_3");
    if (rw >= 0 && rx >= 0 && ry >= 0 && rz >= 0) {
      g.rotation = Eigen::Quaternionf(value(i, rw), value(i, rx), value(i, ry), value(i, rz));
      if (g.rotation.norm() > 1e-6f) g.rotation.normalize();
    }
    g.sh_rest.resize(rest_count);
    for (int k = 0; k < rest_count; ++k) {
      g.sh_rest[k] = value(i, indexOf("f_rest_" + std::to_string(k)));
    }
    out->gaussians.push_back(std::move(g));
  }
  return true;
}

// ----------------------------------------------------------------- .splat ---

bool writeSplat(const std::string& path, const GaussianCloud& cloud) {
  if (cloud.empty()) return false;
  std::vector<uint8_t> bytes;
  bytes.reserve(cloud.size() * 32);

  for (const Gaussian& g : cloud.gaussians) {
    appendRaw(&bytes, g.position.x());
    appendRaw(&bytes, g.position.y());
    appendRaw(&bytes, g.position.z());
    // The format stores linear sigma, not log sigma.
    appendRaw(&bytes, std::exp(g.log_scale.x()));
    appendRaw(&bytes, std::exp(g.log_scale.y()));
    appendRaw(&bytes, std::exp(g.log_scale.z()));

    const std::array<float, 3> rgb = gaussianBaseColor(g);
    bytes.push_back(quantizeUnit(rgb[0]));
    bytes.push_back(quantizeUnit(rgb[1]));
    bytes.push_back(quantizeUnit(rgb[2]));
    bytes.push_back(quantizeUnit(gaussianOpacity(g)));

    // Quaternion components mapped from [-1,1] onto a byte, w first.
    const Eigen::Quaternionf q = g.rotation.normalized();
    const float wxyz[4] = {q.w(), q.x(), q.y(), q.z()};
    for (float c : wxyz) {
      bytes.push_back(static_cast<uint8_t>(
          std::max(0.f, std::min(255.f, std::round(c * 128.0f + 128.0f)))));
    }
  }
  return writeFileBytes(path, bytes);
}

bool readSplat(const std::string& path, GaussianCloud* out, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return fail("cannot read " + path);
  if (bytes.empty() || bytes.size() % 32 != 0) {
    return fail(".splat size is not a multiple of the 32-byte record");
  }

  out->gaussians.clear();
  out->sh_degree = 0;  // the format cannot carry higher orders
  const size_t count = bytes.size() / 32;
  out->gaussians.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    const uint8_t* record = bytes.data() + i * 32;
    float xyz[3], scale[3];
    std::memcpy(xyz, record, 12);
    std::memcpy(scale, record + 12, 12);

    Gaussian g;
    g.position = Vec3f(xyz[0], xyz[1], xyz[2]);
    for (int c = 0; c < 3; ++c) {
      g.log_scale[c] = std::log(std::max(1e-20f, scale[c]));
    }
    for (int c = 0; c < 3; ++c) {
      const float linear = record[24 + c] / 255.0f;
      g.dc[c] = (linear - 0.5f) / kShC0;
    }
    const float alpha = std::max(1e-6f, std::min(1.f - 1e-6f, record[27] / 255.0f));
    g.opacity_logit = std::log(alpha / (1.f - alpha));
    const auto dequant = [](uint8_t b) { return (static_cast<float>(b) - 128.0f) / 128.0f; };
    g.rotation = Eigen::Quaternionf(dequant(record[28]), dequant(record[29]),
                                    dequant(record[30]), dequant(record[31]));
    if (g.rotation.norm() > 1e-6f) {
      g.rotation.normalize();
    } else {
      g.rotation = Eigen::Quaternionf::Identity();
    }
    out->gaussians.push_back(std::move(g));
  }
  return true;
}

// -------------------------------------------------------------------- .spz ---

bool writeSpz(const std::string& path, const GaussianCloud& cloud, int fractional_bits) {
  if (cloud.empty()) return false;
  if (cloud.sh_degree < 0 || cloud.sh_degree > 3) return false;
  fractional_bits = std::max(0, std::min(20, fractional_bits));

  const size_t n = cloud.size();
  const int rest_per_channel = shRestPerChannel(cloud.sh_degree);

  std::vector<uint8_t> raw;
  raw.reserve(16 + n * (9 + 3 + 3 + 1 + 3 + rest_per_channel * 3));

  appendRaw(&raw, kSpzMagic);
  appendRaw(&raw, kSpzVersion);
  appendRaw(&raw, static_cast<uint32_t>(n));
  raw.push_back(static_cast<uint8_t>(cloud.sh_degree));
  raw.push_back(static_cast<uint8_t>(fractional_bits));
  raw.push_back(static_cast<uint8_t>(cloud.antialiased ? 0x1 : 0x0));
  raw.push_back(0);  // reserved

  // Attributes are stored as planes, in this order, not interleaved per splat.
  for (const Gaussian& g : cloud.gaussians) {
    spzEncodePosition(g.position.x(), fractional_bits, &raw);
    spzEncodePosition(g.position.y(), fractional_bits, &raw);
    spzEncodePosition(g.position.z(), fractional_bits, &raw);
  }
  for (const Gaussian& g : cloud.gaussians) {
    for (int c = 0; c < 3; ++c) raw.push_back(spzEncodeScale(g.log_scale[c]));
  }
  for (const Gaussian& g : cloud.gaussians) {
    // Only x, y, z are stored; w is recovered from the unit-norm constraint, so
    // the quaternion is first put into the hemisphere where w >= 0.
    Eigen::Quaternionf q = g.rotation.normalized();
    if (q.w() < 0.f) q.coeffs() *= -1.f;
    raw.push_back(spzEncodeQuatComponent(q.x()));
    raw.push_back(spzEncodeQuatComponent(q.y()));
    raw.push_back(spzEncodeQuatComponent(q.z()));
  }
  for (const Gaussian& g : cloud.gaussians) {
    raw.push_back(quantizeUnit(gaussianOpacity(g)));
  }
  for (const Gaussian& g : cloud.gaussians) {
    for (int c = 0; c < 3; ++c) raw.push_back(spzEncodeColor(g.dc[c]));
  }
  for (const Gaussian& g : cloud.gaussians) {
    // The colour channel is the faster-varying axis here, so the channel-major
    // storage used in memory has to be transposed on the way out.
    for (int k = 0; k < rest_per_channel; ++k) {
      for (int c = 0; c < 3; ++c) {
        const size_t index = static_cast<size_t>(c) * rest_per_channel + k;
        raw.push_back(spzEncodeSh(index < g.sh_rest.size() ? g.sh_rest[index] : 0.f));
      }
    }
  }

  std::vector<uint8_t> compressed;
  if (!gzipCompress(raw, &compressed)) return false;
  return writeFileBytes(path, compressed);
}

bool readSpz(const std::string& path, GaussianCloud* out, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  std::vector<uint8_t> compressed;
  if (!readFileBytes(path, &compressed)) return fail("cannot read " + path);
  std::vector<uint8_t> raw;
  if (!gzipDecompress(compressed, &raw)) return fail(".spz is not a valid gzip stream");
  if (raw.size() < 16) return fail(".spz header is truncated");

  size_t pos = 0;
  uint32_t magic = 0, version = 0, count = 0;
  if (!takeRaw(raw, &pos, &magic) || magic != kSpzMagic) return fail("bad .spz magic");
  if (!takeRaw(raw, &pos, &version) || version != kSpzVersion) {
    return fail("unsupported .spz version");
  }
  if (!takeRaw(raw, &pos, &count)) return fail(".spz header is truncated");
  const uint8_t sh_degree = raw[pos++];
  const uint8_t fractional_bits = raw[pos++];
  const uint8_t flags = raw[pos++];
  ++pos;  // reserved
  if (sh_degree > 3) return fail(".spz sh_degree out of range");

  const int rest_per_channel = shRestPerChannel(sh_degree);
  const size_t per_splat = 9 + 3 + 3 + 1 + 3 + static_cast<size_t>(rest_per_channel) * 3;
  // Validate the declared count against the payload before allocating.
  if (raw.size() - pos != static_cast<size_t>(count) * per_splat) {
    return fail(".spz payload size does not match the declared point count");
  }

  out->gaussians.assign(count, Gaussian{});
  out->sh_degree = sh_degree;
  out->antialiased = (flags & 0x1) != 0;

  for (uint32_t i = 0; i < count; ++i) {
    Gaussian& g = out->gaussians[i];
    const uint8_t* p = raw.data() + pos + static_cast<size_t>(i) * 9;
    g.position = Vec3f(spzDecodePosition(p, fractional_bits),
                       spzDecodePosition(p + 3, fractional_bits),
                       spzDecodePosition(p + 6, fractional_bits));
  }
  pos += static_cast<size_t>(count) * 9;

  for (uint32_t i = 0; i < count; ++i) {
    for (int c = 0; c < 3; ++c) {
      out->gaussians[i].log_scale[c] = spzDecodeScale(raw[pos + i * 3 + c]);
    }
  }
  pos += static_cast<size_t>(count) * 3;

  for (uint32_t i = 0; i < count; ++i) {
    const float x = spzDecodeQuatComponent(raw[pos + i * 3 + 0]);
    const float y = spzDecodeQuatComponent(raw[pos + i * 3 + 1]);
    const float z = spzDecodeQuatComponent(raw[pos + i * 3 + 2]);
    // Recover w from the unit-norm constraint; the writer guaranteed w >= 0.
    const float w = std::sqrt(std::max(0.f, 1.f - x * x - y * y - z * z));
    Eigen::Quaternionf q(w, x, y, z);
    out->gaussians[i].rotation = q.norm() > 1e-6f ? q.normalized()
                                                  : Eigen::Quaternionf::Identity();
  }
  pos += static_cast<size_t>(count) * 3;

  for (uint32_t i = 0; i < count; ++i) {
    const float alpha = std::max(1e-6f, std::min(1.f - 1e-6f, raw[pos + i] / 255.0f));
    out->gaussians[i].opacity_logit = std::log(alpha / (1.f - alpha));
  }
  pos += count;

  for (uint32_t i = 0; i < count; ++i) {
    for (int c = 0; c < 3; ++c) {
      out->gaussians[i].dc[c] = spzDecodeColor(raw[pos + i * 3 + c]);
    }
  }
  pos += static_cast<size_t>(count) * 3;

  const size_t rest_total = static_cast<size_t>(rest_per_channel) * 3;
  for (uint32_t i = 0; i < count; ++i) {
    Gaussian& g = out->gaussians[i];
    g.sh_rest.assign(rest_total, 0.f);
    for (int k = 0; k < rest_per_channel; ++k) {
      for (int c = 0; c < 3; ++c) {
        // Undo the channel-interleaved storage.
        g.sh_rest[static_cast<size_t>(c) * rest_per_channel + k] =
            spzDecodeSh(raw[pos + (static_cast<size_t>(i) * rest_per_channel + k) * 3 + c]);
      }
    }
  }
  return true;
}

}  // namespace parallax
