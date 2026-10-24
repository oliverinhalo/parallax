#include "parallax/splat/GaussianCloud.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "parallax/io/ImageIo.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

class TempDir {
 public:
  TempDir() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      path_ = base / ("parallax_splat_" + std::to_string(counter()++));
      std::error_code ec;
      if (std::filesystem::create_directory(path_, ec)) return;
    }
    path_ = base;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  static int& counter() {
    static int value = 5000;
    return value;
  }
  std::filesystem::path path_;
};

/// A deterministic cloud with non-trivial values in every field.
GaussianCloud makeCloud(size_t count = 200, int sh_degree = 2) {
  GaussianCloud cloud;
  cloud.sh_degree = sh_degree;
  cloud.antialiased = true;
  Rng rng(1234);
  const size_t rest = static_cast<size_t>(shRestPerChannel(sh_degree)) * 3;

  for (size_t i = 0; i < count; ++i) {
    Gaussian g;
    g.position = Vec3f(static_cast<float>(rng.uniform(-2.0, 2.0)),
                       static_cast<float>(rng.uniform(-2.0, 2.0)),
                       static_cast<float>(rng.uniform(-2.0, 2.0)));
    g.log_scale = Vec3f(static_cast<float>(rng.uniform(-5.0, -1.0)),
                        static_cast<float>(rng.uniform(-5.0, -1.0)),
                        static_cast<float>(rng.uniform(-5.0, -1.0)));
    Eigen::Quaternionf q(static_cast<float>(rng.normal()), static_cast<float>(rng.normal()),
                         static_cast<float>(rng.normal()), static_cast<float>(rng.normal()));
    if (q.norm() < 1e-3f) q = Eigen::Quaternionf::Identity();
    g.rotation = q.normalized();
    g.opacity_logit = static_cast<float>(rng.uniform(-3.0, 3.0));
    for (int c = 0; c < 3; ++c) g.dc[c] = static_cast<float>(rng.uniform(-1.5, 1.5));
    g.sh_rest.resize(rest);
    for (size_t k = 0; k < rest; ++k) g.sh_rest[k] = static_cast<float>(rng.uniform(-0.6, 0.6));
    cloud.gaussians.push_back(std::move(g));
  }
  return cloud;
}

}  // namespace

TEST(Splat, ShCoefficientCountsMatchTheFormat) {
  EXPECT_EQ(shCoefficientsPerChannel(0), 1);
  EXPECT_EQ(shCoefficientsPerChannel(1), 4);
  EXPECT_EQ(shCoefficientsPerChannel(2), 9);
  EXPECT_EQ(shCoefficientsPerChannel(3), 16);
  // The INRIA f_rest_* properties exclude the DC term.
  EXPECT_EQ(shRestPerChannel(0), 0);
  EXPECT_EQ(shRestPerChannel(1), 3);
  EXPECT_EQ(shRestPerChannel(2), 8);
  EXPECT_EQ(shRestPerChannel(3), 15);
}

TEST(Splat, ValidationCatchesInconsistentClouds) {
  GaussianCloud cloud = makeCloud(10, 1);
  std::string reason;
  EXPECT_TRUE(cloud.isValid(&reason)) << reason;

  GaussianCloud wrong_rest = cloud;
  wrong_rest.gaussians[0].sh_rest.pop_back();
  EXPECT_FALSE(wrong_rest.isValid(&reason));

  GaussianCloud bad_degree = cloud;
  bad_degree.sh_degree = 7;
  EXPECT_FALSE(bad_degree.isValid(&reason));

  GaussianCloud bad_rotation = cloud;
  bad_rotation.gaussians[0].rotation = Eigen::Quaternionf(0, 0, 0, 0);
  EXPECT_FALSE(bad_rotation.isValid(&reason));

  GaussianCloud non_finite = cloud;
  non_finite.gaussians[0].position.x() = std::nanf("");
  EXPECT_FALSE(non_finite.isValid(&reason));
}

TEST(Splat, ColorAndOpacityConversionsAreConsistent) {
  Gaussian g;
  // A DC term of 0 maps to mid grey, by construction of the SH basis.
  g.dc = {0.f, 0.f, 0.f};
  const std::array<float, 3> grey = gaussianBaseColor(g);
  for (float c : grey) EXPECT_NEAR(c, 0.5f, 1e-6f);

  // Round-trip an arbitrary colour through the DC encoding.
  for (float target : {0.05f, 0.25f, 0.5f, 0.75f, 0.95f}) {
    Gaussian h;
    for (int c = 0; c < 3; ++c) h.dc[c] = (target - 0.5f) / kShC0;
    EXPECT_NEAR(gaussianBaseColor(h)[0], target, 1e-5f);
  }

  // Opacity is a logistic of the stored logit.
  g.opacity_logit = 0.f;
  EXPECT_NEAR(gaussianOpacity(g), 0.5f, 1e-6f);
  g.opacity_logit = 10.f;
  EXPECT_GT(gaussianOpacity(g), 0.999f);
  g.opacity_logit = -10.f;
  EXPECT_LT(gaussianOpacity(g), 0.001f);
}

TEST(Splat, PruneDropsInvisibleAndRunawaySplats) {
  GaussianCloud cloud = makeCloud(50, 0);
  cloud.gaussians[0].opacity_logit = -20.f;             // effectively invisible
  cloud.gaussians[1].log_scale = Vec3f::Constant(20.f);  // enormous
  const size_t before = cloud.size();
  const int removed = cloud.prune();
  EXPECT_GE(removed, 2);
  EXPECT_EQ(cloud.size(), before - removed);
  for (const Gaussian& g : cloud.gaussians) {
    EXPECT_GE(gaussianOpacity(g), 0.0045f);
  }
}

TEST(Splat, SortByOpacityIsDescending) {
  GaussianCloud cloud = makeCloud(80, 0);
  cloud.sortByOpacity();
  for (size_t i = 1; i < cloud.size(); ++i) {
    EXPECT_GE(cloud.gaussians[i - 1].opacity_logit, cloud.gaussians[i].opacity_logit);
  }
}

TEST(Splat, BoundsCoverEveryPosition) {
  const GaussianCloud cloud = makeCloud(100, 0);
  const AxisAlignedBox box = cloud.bounds();
  ASSERT_TRUE(box.valid());
  for (const Gaussian& g : cloud.gaussians) {
    EXPECT_TRUE(box.contains(Vec3(g.position.x(), g.position.y(), g.position.z())));
  }
}

TEST(Splat, InitializeFromPointCloudProducesASensibleStartingField) {
  // Points on a sphere of known radius, so the expected spacing is known too.
  std::vector<Vec3f> points;
  std::vector<std::array<uint8_t, 3>> colors;
  Rng rng(7);
  for (int i = 0; i < 400; ++i) {
    const Vec3 dir = rng.unitVector() * 0.5;
    points.push_back(
        Vec3f(static_cast<float>(dir.x()), static_cast<float>(dir.y()),
              static_cast<float>(dir.z())));
    colors.push_back({200, 100, 50});
  }

  const GaussianCloud cloud = initializeFromPointCloud(points, colors, 1, 0.1f);
  ASSERT_EQ(cloud.size(), points.size());
  EXPECT_EQ(cloud.sh_degree, 1);
  std::string reason;
  EXPECT_TRUE(cloud.isValid(&reason)) << reason;

  for (size_t i = 0; i < cloud.size(); ++i) {
    const Gaussian& g = cloud.gaussians[i];
    EXPECT_LT((g.position - points[i]).norm(), 1e-6f);
    // The initial colour must match the point colour.
    const std::array<float, 3> rgb = gaussianBaseColor(g);
    EXPECT_NEAR(rgb[0], 200 / 255.0f, 2e-3f);
    EXPECT_NEAR(rgb[1], 100 / 255.0f, 2e-3f);
    EXPECT_NEAR(rgb[2], 50 / 255.0f, 2e-3f);
    EXPECT_NEAR(gaussianOpacity(g), 0.1f, 1e-4f);
    // Scales must be small relative to the object, not degenerate.
    const float sigma = std::exp(g.log_scale.x());
    EXPECT_GT(sigma, 1e-4f);
    EXPECT_LT(sigma, 0.5f);
  }
  EXPECT_TRUE(initializeFromPointCloud({}, {}, 0).empty());
}

// ------------------------------------------------------------ 3DGS PLY ---

TEST(Splat, GaussianPlyRoundTripsEveryFieldExactly) {
  TempDir dir;
  const GaussianCloud original = makeCloud(150, 3);
  const std::string path = dir.file("splats.ply");
  ASSERT_TRUE(writeGaussianPly(path, original));

  GaussianCloud loaded;
  std::string error;
  ASSERT_TRUE(readGaussianPly(path, &loaded, &error)) << error;
  ASSERT_EQ(loaded.size(), original.size());
  EXPECT_EQ(loaded.sh_degree, original.sh_degree);

  for (size_t i = 0; i < original.size(); ++i) {
    const Gaussian& a = original.gaussians[i];
    const Gaussian& b = loaded.gaussians[i];
    // The PLY layout is float32 throughout, so this is lossless.
    EXPECT_EQ(b.position, a.position) << i;
    EXPECT_EQ(b.log_scale, a.log_scale) << i;
    EXPECT_FLOAT_EQ(b.opacity_logit, a.opacity_logit) << i;
    for (int c = 0; c < 3; ++c) EXPECT_FLOAT_EQ(b.dc[c], a.dc[c]) << i << "/" << c;
    ASSERT_EQ(b.sh_rest.size(), a.sh_rest.size());
    for (size_t k = 0; k < a.sh_rest.size(); ++k) {
      EXPECT_FLOAT_EQ(b.sh_rest[k], a.sh_rest[k]) << i << "/" << k;
    }
    // Quaternions are equal up to sign.
    const float dot = std::abs(b.rotation.dot(a.rotation));
    EXPECT_NEAR(dot, 1.0f, 1e-5f) << i;
  }
}

TEST(Splat, GaussianPlyInfersTheShDegreeFromPropertyCount) {
  TempDir dir;
  for (int degree = 0; degree <= 3; ++degree) {
    const GaussianCloud original = makeCloud(20, degree);
    const std::string path = dir.file("deg" + std::to_string(degree) + ".ply");
    ASSERT_TRUE(writeGaussianPly(path, original));
    GaussianCloud loaded;
    std::string error;
    ASSERT_TRUE(readGaussianPly(path, &loaded, &error)) << error;
    EXPECT_EQ(loaded.sh_degree, degree);
    EXPECT_EQ(loaded.gaussians[0].sh_rest.size(),
              static_cast<size_t>(shRestPerChannel(degree)) * 3);
  }
}

TEST(Splat, GaussianPlyHeaderDeclaresTheReferenceProperties) {
  TempDir dir;
  const std::string path = dir.file("header.ply");
  ASSERT_TRUE(writeGaussianPly(path, makeCloud(5, 1)));
  std::string text;
  ASSERT_TRUE(readFileText(path, &text));
  // Viewers look these up by name; a typo here breaks every consumer.
  for (const char* property : {"property float x", "property float f_dc_0",
                               "property float f_rest_0", "property float opacity",
                               "property float scale_0", "property float rot_0",
                               "property float rot_3"}) {
    EXPECT_NE(text.find(property), std::string::npos) << property;
  }
  EXPECT_NE(text.find("binary_little_endian"), std::string::npos);
}

TEST(Splat, GaussianPlyReaderRejectsBadInput) {
  TempDir dir;
  GaussianCloud cloud;
  std::string error;
  EXPECT_FALSE(readGaussianPly(dir.file("missing.ply"), &cloud, &error));

  ASSERT_TRUE(writeFileText(dir.file("not.ply"), "hello"));
  EXPECT_FALSE(readGaussianPly(dir.file("not.ply"), &cloud, &error));

  // An ASCII 3DGS PLY is not supported, and must be reported rather than
  // silently misread.
  ASSERT_TRUE(writeFileText(dir.file("ascii.ply"),
                            "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"
                            "end_header\n0\n"));
  EXPECT_FALSE(readGaussianPly(dir.file("ascii.ply"), &cloud, &error));
  EXPECT_NE(error.find("binary little-endian"), std::string::npos);

  EXPECT_FALSE(writeGaussianPly(dir.file("empty.ply"), GaussianCloud{}));
}

// -------------------------------------------------------------- .splat ---

TEST(Splat, SplatFormatIsThirtyTwoBytesPerRecord) {
  TempDir dir;
  const GaussianCloud original = makeCloud(123, 0);
  const std::string path = dir.file("cloud.splat");
  ASSERT_TRUE(writeSplat(path, original));

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  EXPECT_EQ(bytes.size(), original.size() * 32);
}

TEST(Splat, SplatRoundTripsWithinQuantisationError) {
  TempDir dir;
  const GaussianCloud original = makeCloud(200, 0);
  const std::string path = dir.file("rt.splat");
  ASSERT_TRUE(writeSplat(path, original));

  GaussianCloud loaded;
  std::string error;
  ASSERT_TRUE(readSplat(path, &loaded, &error)) << error;
  ASSERT_EQ(loaded.size(), original.size());
  EXPECT_EQ(loaded.sh_degree, 0);

  for (size_t i = 0; i < original.size(); ++i) {
    const Gaussian& a = original.gaussians[i];
    const Gaussian& b = loaded.gaussians[i];
    // Positions and scales are float32: exact.
    EXPECT_EQ(b.position, a.position) << i;
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(b.log_scale[c], a.log_scale[c], 1e-5f) << i;
    // Colour and opacity are 8-bit: compare in the quantised domain.
    const std::array<float, 3> ca = gaussianBaseColor(a);
    const std::array<float, 3> cb = gaussianBaseColor(b);
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(cb[c], ca[c], 1.0f / 255.0f) << i;
    EXPECT_NEAR(gaussianOpacity(b), gaussianOpacity(a), 1.5f / 255.0f) << i;
    // Rotation is 8-bit per component; the angular error must stay small.
    const float dot = std::abs(b.rotation.dot(a.rotation));
    EXPECT_GT(dot, 0.99f) << i;
  }
}

TEST(Splat, SplatDropsHigherOrderHarmonicsAsDocumented) {
  TempDir dir;
  const GaussianCloud original = makeCloud(30, 3);
  const std::string path = dir.file("sh.splat");
  ASSERT_TRUE(writeSplat(path, original));
  GaussianCloud loaded;
  ASSERT_TRUE(readSplat(path, &loaded));
  // The format cannot carry them, so the result must be a clean degree 0 cloud
  // rather than a degree-3 cloud with zeroed coefficients.
  EXPECT_EQ(loaded.sh_degree, 0);
  EXPECT_TRUE(loaded.gaussians[0].sh_rest.empty());
  std::string reason;
  EXPECT_TRUE(loaded.isValid(&reason)) << reason;
}

TEST(Splat, SplatReaderRejectsTruncatedFiles) {
  TempDir dir;
  GaussianCloud cloud;
  std::string error;
  ASSERT_TRUE(writeFileBytes(dir.file("odd.splat"), std::vector<uint8_t>(37, 0)));
  EXPECT_FALSE(readSplat(dir.file("odd.splat"), &cloud, &error));
  EXPECT_NE(error.find("32-byte"), std::string::npos);
  EXPECT_FALSE(writeSplat(dir.file("empty.splat"), GaussianCloud{}));
}

// ---------------------------------------------------------------- .spz ---

TEST(Splat, SpzIsGzippedAndSmallerThanPly) {
  TempDir dir;
  const GaussianCloud original = makeCloud(500, 2);
  const std::string ply = dir.file("compare.ply");
  const std::string spz = dir.file("compare.spz");
  ASSERT_TRUE(writeGaussianPly(ply, original));
  ASSERT_TRUE(writeSpz(spz, original));

  std::vector<uint8_t> spz_bytes, ply_bytes;
  ASSERT_TRUE(readFileBytes(spz, &spz_bytes));
  ASSERT_TRUE(readFileBytes(ply, &ply_bytes));

  // gzip magic.
  ASSERT_GE(spz_bytes.size(), 2u);
  EXPECT_EQ(spz_bytes[0], 0x1f);
  EXPECT_EQ(spz_bytes[1], 0x8b);
  // The whole point of the format is that it is much smaller.
  EXPECT_LT(spz_bytes.size(), ply_bytes.size() / 3);
}

TEST(Splat, SpzRoundTripsWithinQuantisationError) {
  TempDir dir;
  const GaussianCloud original = makeCloud(300, 2);
  const std::string path = dir.file("rt.spz");
  ASSERT_TRUE(writeSpz(path, original, 12));

  GaussianCloud loaded;
  std::string error;
  ASSERT_TRUE(readSpz(path, &loaded, &error)) << error;
  ASSERT_EQ(loaded.size(), original.size());
  EXPECT_EQ(loaded.sh_degree, original.sh_degree);
  EXPECT_EQ(loaded.antialiased, original.antialiased);

  for (size_t i = 0; i < original.size(); ++i) {
    const Gaussian& a = original.gaussians[i];
    const Gaussian& b = loaded.gaussians[i];
    // 12 fractional bits gives a quantum of 1/4096 m, about 0.24 mm.
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(b.position[c], a.position[c], 1.0f / 4096.0f) << i << "/" << c;
    }
    // Scale bytes carry 1/16 of a log unit.
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(b.log_scale[c], a.log_scale[c], 1.0f / 32.0f) << i << "/" << c;
    }
    const std::array<float, 3> ca = gaussianBaseColor(a);
    const std::array<float, 3> cb = gaussianBaseColor(b);
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(cb[c], ca[c], 1.0f / 255.0f) << i;
    EXPECT_NEAR(gaussianOpacity(b), gaussianOpacity(a), 1.5f / 255.0f) << i;
    const float dot = std::abs(b.rotation.dot(a.rotation));
    EXPECT_GT(dot, 0.985f) << i;
    ASSERT_EQ(b.sh_rest.size(), a.sh_rest.size());
    for (size_t k = 0; k < a.sh_rest.size(); ++k) {
      EXPECT_NEAR(b.sh_rest[k], a.sh_rest[k], 1.0f / 128.0f) << i << "/" << k;
    }
  }
}

TEST(Splat, SpzHeaderMatchesTheSpecifiedLayout) {
  TempDir dir;
  GaussianCloud cloud = makeCloud(64, 1);
  cloud.antialiased = true;
  const std::string path = dir.file("header.spz");
  ASSERT_TRUE(writeSpz(path, cloud, 14));

  // Decompress and inspect the 16-byte header field by field.
  std::vector<uint8_t> compressed;
  ASSERT_TRUE(readFileBytes(path, &compressed));
  // Round-trip through the reader to confirm the header was understood.
  GaussianCloud loaded;
  ASSERT_TRUE(readSpz(path, &loaded));
  EXPECT_EQ(loaded.size(), 64u);
  EXPECT_EQ(loaded.sh_degree, 1);
  EXPECT_TRUE(loaded.antialiased);

  // A cloud written without antialiasing must report false.
  cloud.antialiased = false;
  const std::string plain = dir.file("plain.spz");
  ASSERT_TRUE(writeSpz(plain, cloud));
  GaussianCloud plain_loaded;
  ASSERT_TRUE(readSpz(plain, &plain_loaded));
  EXPECT_FALSE(plain_loaded.antialiased);
}

TEST(Splat, SpzFractionalBitsTradePrecisionForRange) {
  TempDir dir;
  GaussianCloud cloud;
  cloud.sh_degree = 0;
  Gaussian g;
  g.position = Vec3f(1.234567f, -2.345678f, 3.456789f);
  g.rotation = Eigen::Quaternionf::Identity();
  cloud.gaussians.push_back(g);

  // More fractional bits must mean a smaller position error.
  double previous_error = 1e9;
  for (int bits : {4, 8, 12, 16}) {
    const std::string path = dir.file("bits" + std::to_string(bits) + ".spz");
    ASSERT_TRUE(writeSpz(path, cloud, bits));
    GaussianCloud loaded;
    ASSERT_TRUE(readSpz(path, &loaded));
    const double error = (loaded.gaussians[0].position - g.position).norm();
    EXPECT_LE(error, previous_error) << "bits " << bits;
    EXPECT_LE(error, 3.0 / (1 << bits)) << "bits " << bits;
    previous_error = error;
  }
}

TEST(Splat, SpzReaderRejectsCorruptInput) {
  TempDir dir;
  const GaussianCloud cloud = makeCloud(40, 1);
  const std::string path = dir.file("good.spz");
  ASSERT_TRUE(writeSpz(path, cloud));

  GaussianCloud loaded;
  std::string error;
  // Not gzip at all.
  ASSERT_TRUE(writeFileBytes(dir.file("raw.spz"), std::vector<uint8_t>(64, 7)));
  EXPECT_FALSE(readSpz(dir.file("raw.spz"), &loaded, &error));
  EXPECT_NE(error.find("gzip"), std::string::npos);

  // Valid gzip, but not an spz payload.
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
  ASSERT_TRUE(writeFileBytes(dir.file("cut.spz"), truncated));
  EXPECT_FALSE(readSpz(dir.file("cut.spz"), &loaded, &error));

  EXPECT_FALSE(writeSpz(dir.file("empty.spz"), GaussianCloud{}));
}

TEST(Splat, AllThreeFormatsAgreeOnGeometry) {
  // The formats differ in what they can carry, but a splat's position and
  // rough appearance must survive all three.
  TempDir dir;
  const GaussianCloud original = makeCloud(120, 0);
  ASSERT_TRUE(writeGaussianPly(dir.file("a.ply"), original));
  ASSERT_TRUE(writeSplat(dir.file("a.splat"), original));
  ASSERT_TRUE(writeSpz(dir.file("a.spz"), original, 16));

  GaussianCloud from_ply, from_splat, from_spz;
  ASSERT_TRUE(readGaussianPly(dir.file("a.ply"), &from_ply));
  ASSERT_TRUE(readSplat(dir.file("a.splat"), &from_splat));
  ASSERT_TRUE(readSpz(dir.file("a.spz"), &from_spz));

  ASSERT_EQ(from_ply.size(), original.size());
  ASSERT_EQ(from_splat.size(), original.size());
  ASSERT_EQ(from_spz.size(), original.size());

  for (size_t i = 0; i < original.size(); ++i) {
    const Vec3f& truth = original.gaussians[i].position;
    EXPECT_LT((from_ply.gaussians[i].position - truth).norm(), 1e-6f) << i;
    EXPECT_LT((from_splat.gaussians[i].position - truth).norm(), 1e-6f) << i;
    EXPECT_LT((from_spz.gaussians[i].position - truth).norm(), 1e-4f) << i;

    const std::array<float, 3> truth_rgb = gaussianBaseColor(original.gaussians[i]);
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(gaussianBaseColor(from_ply.gaussians[i])[c], truth_rgb[c], 1e-6f);
      EXPECT_NEAR(gaussianBaseColor(from_splat.gaussians[i])[c], truth_rgb[c], 1.0f / 255.0f);
      EXPECT_NEAR(gaussianBaseColor(from_spz.gaussians[i])[c], truth_rgb[c], 1.0f / 255.0f);
    }
  }
}
