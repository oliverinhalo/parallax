#include "parallax/io/JpegIo.h"

#include "fixtures/JpegFixtures.h"
#include "parallax/io/ImageIo.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>

namespace parallax {
namespace {

std::vector<uint8_t> asBytes(const unsigned char* data, size_t size) {
  return std::vector<uint8_t>(data, data + size);
}

struct Difference {
  int max_absolute = 0;
  double mean_absolute = 0.0;
};

Difference compare(const ColorBuffer& decoded, const unsigned char* expected) {
  Difference difference;
  long long total = 0;
  const size_t count = static_cast<size_t>(decoded.width) * decoded.height * 3;
  for (size_t i = 0; i < count; ++i) {
    const int delta = std::abs(static_cast<int>(decoded.pixels[i]) -
                               static_cast<int>(expected[i]));
    difference.max_absolute = std::max(difference.max_absolute, delta);
    total += delta;
  }
  difference.mean_absolute = count == 0 ? 0.0 : static_cast<double>(total) / count;
  return difference;
}

/**
 * Every fixture decodes to what libjpeg-turbo decodes it to.
 *
 * The tolerance exists because the IDCT is not specified exactly: the standard
 * defines the transform mathematically and lets implementations choose their
 * fixed-point arrangement, so two correct decoders differ by a code value or
 * two on individual pixels. A *mean* error under a code value is the real
 * check -- a wrong zig-zag order, a wrong quantisation table or a chroma plane
 * off by one block all push the mean far past this.
 */
TEST(JpegTest, MatchesReferenceDecoder) {
  ASSERT_GT(jpeg_fixtures::kFixtureCount, 0);
  for (int i = 0; i < jpeg_fixtures::kFixtureCount; ++i) {
    const auto& fixture = jpeg_fixtures::kFixtures[i];
    SCOPED_TRACE(fixture.name);

    ColorBuffer decoded;
    std::string error;
    ASSERT_TRUE(decodeJpeg(asBytes(fixture.jpeg, fixture.jpeg_size), &decoded, &error))
        << error;
    ASSERT_EQ(decoded.width, fixture.width);
    ASSERT_EQ(decoded.height, fixture.height);

    const Difference difference = compare(decoded, fixture.rgb);
    EXPECT_LE(difference.mean_absolute, 1.0)
        << "mean error " << difference.mean_absolute;
    EXPECT_LE(difference.max_absolute, 12) << "max error " << difference.max_absolute;
  }
}

TEST(JpegTest, ReadsDimensionsWithoutDecoding) {
  for (int i = 0; i < jpeg_fixtures::kFixtureCount; ++i) {
    const auto& fixture = jpeg_fixtures::kFixtures[i];
    SCOPED_TRACE(fixture.name);
    int width = 0, height = 0;
    ASSERT_TRUE(readJpegSize(asBytes(fixture.jpeg, fixture.jpeg_size), &width, &height));
    EXPECT_EQ(width, fixture.width);
    EXPECT_EQ(height, fixture.height);
  }
}

TEST(JpegTest, DetectsTheMagicBytes) {
  const auto& fixture = jpeg_fixtures::kFixtures[0];
  EXPECT_TRUE(looksLikeJpeg(asBytes(fixture.jpeg, fixture.jpeg_size)));
  EXPECT_FALSE(looksLikeJpeg({}));
  EXPECT_FALSE(looksLikeJpeg({0x89, 'P', 'N', 'G'}));
  EXPECT_FALSE(looksLikeJpeg({0xFF}));
}

/// A greyscale JPEG still decodes to three equal channels, so callers never
/// have to branch on the component count.
TEST(JpegTest, GreyscaleDecodesToNeutralRgb) {
  const jpeg_fixtures::Fixture* grey = nullptr;
  for (int i = 0; i < jpeg_fixtures::kFixtureCount; ++i) {
    if (std::string(jpeg_fixtures::kFixtures[i].name) == "grey") {
      grey = &jpeg_fixtures::kFixtures[i];
    }
  }
  ASSERT_NE(grey, nullptr);

  ColorBuffer decoded;
  ASSERT_TRUE(decodeJpeg(asBytes(grey->jpeg, grey->jpeg_size), &decoded));
  for (int y = 0; y < decoded.height; ++y) {
    for (int x = 0; x < decoded.width; ++x) {
      uint8_t rgb[3];
      decoded.get(x, y, rgb);
      EXPECT_EQ(rgb[0], rgb[1]);
      EXPECT_EQ(rgb[1], rgb[2]);
    }
  }
}

/// Flat colour means every AC coefficient is zero, which takes the IDCT's
/// DC-only shortcut. If that path were wrong the image would still look
/// plausible, so it is checked against the exact colour.
TEST(JpegTest, FlatColourIsExact) {
  const jpeg_fixtures::Fixture* flat = nullptr;
  for (int i = 0; i < jpeg_fixtures::kFixtureCount; ++i) {
    if (std::string(jpeg_fixtures::kFixtures[i].name) == "flat") {
      flat = &jpeg_fixtures::kFixtures[i];
    }
  }
  ASSERT_NE(flat, nullptr);

  ColorBuffer decoded;
  ASSERT_TRUE(decodeJpeg(asBytes(flat->jpeg, flat->jpeg_size), &decoded));
  const Difference difference = compare(decoded, flat->rgb);
  EXPECT_LE(difference.max_absolute, 2);
}

TEST(JpegTest, RejectsNonJpegWithAReason) {
  std::string error;
  ColorBuffer decoded;
  EXPECT_FALSE(decodeJpeg({0x89, 'P', 'N', 'G'}, &decoded, &error));
  EXPECT_FALSE(error.empty());
}

TEST(JpegTest, RejectsProgressiveJpegRatherThanDecodingItWrong) {
  // A minimal SOF2 header. Decoding a progressive scan as baseline produces a
  // recognisable but wrong image, which is far worse than an error.
  std::vector<uint8_t> progressive = {
      0xFF, 0xD8,                                      // SOI
      0xFF, 0xC2, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00,  // SOF2, 16x16
      0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01,
      0x03, 0x11, 0x01,
      0xFF, 0xD9,
  };
  ColorBuffer decoded;
  std::string error;
  EXPECT_FALSE(decodeJpeg(progressive, &decoded, &error));
  EXPECT_NE(error.find("rogressive"), std::string::npos) << error;
}

TEST(JpegTest, RejectsTruncatedData) {
  const auto& fixture = jpeg_fixtures::kFixtures[0];
  // Cut before the scan header: no image data at all.
  std::vector<uint8_t> truncated =
      asBytes(fixture.jpeg, std::min<size_t>(fixture.jpeg_size, 40));
  ColorBuffer decoded;
  std::string error;
  EXPECT_FALSE(decodeJpeg(truncated, &decoded, &error));
  EXPECT_FALSE(error.empty());
}

/// A file cut off mid-scan must not crash or read out of bounds. It may decode
/// the part it has; what matters is that it terminates and stays in bounds.
TEST(JpegTest, SurvivesTruncationMidScan) {
  const auto& fixture = jpeg_fixtures::kFixtures[0];
  for (size_t fraction = 60; fraction < 100; fraction += 10) {
    const size_t size = fixture.jpeg_size * fraction / 100;
    ColorBuffer decoded;
    std::string error;
    decodeJpeg(asBytes(fixture.jpeg, size), &decoded, &error);
    if (!decoded.empty()) {
      EXPECT_EQ(decoded.width, fixture.width);
      EXPECT_EQ(decoded.height, fixture.height);
    }
  }
}

/// Byte-level corruption must not escape the buffers either. The decoder is
/// reading files that arrive from the camera, from a share sheet and from
/// `.parallax` bundles, so hostile input is a real case.
TEST(JpegTest, SurvivesCorruption) {
  const auto& fixture = jpeg_fixtures::kFixtures[0];
  std::vector<uint8_t> bytes = asBytes(fixture.jpeg, fixture.jpeg_size);
  // A deterministic sweep rather than random bytes, so a failure reproduces.
  for (size_t offset = 20; offset < bytes.size(); offset += 37) {
    std::vector<uint8_t> damaged = bytes;
    damaged[offset] ^= 0xFF;
    ColorBuffer decoded;
    std::string error;
    decodeJpeg(damaged, &decoded, &error);
  }
}

/// The frame loader has to accept either format, because PNG is what the
/// engine's own tests and exports produce while JPEG is what capture writes.
TEST(JpegTest, SitsAlongsidePngDetection) {
  ColorBuffer image(4, 4);
  for (int i = 0; i < 4 * 4 * 3; ++i) image.pixels[i] = static_cast<uint8_t>(i * 5);
  std::vector<uint8_t> png;
  ASSERT_TRUE(encodePng(image, &png));
  EXPECT_FALSE(looksLikeJpeg(png));

  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(png, &decoded));
  EXPECT_EQ(decoded.width, 4);
}

}  // namespace
}  // namespace parallax
