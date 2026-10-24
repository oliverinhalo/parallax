#include "parallax/io/Json.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "parallax/io/ImageIo.h"
#include "parallax/io/Zip.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

class TempDir {
 public:
  TempDir() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      path_ = base / ("parallax_io_" + std::to_string(counter()++));
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
    static int value = 9000;
    return value;
  }
  std::filesystem::path path_;
};

ColorBuffer gradient(int w, int h) {
  ColorBuffer image(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      image.set(x, y, static_cast<uint8_t>(x * 255 / std::max(1, w - 1)),
                static_cast<uint8_t>(y * 255 / std::max(1, h - 1)),
                static_cast<uint8_t>(((x / 4 + y / 4) & 1) ? 250 : 5));
    }
  }
  return image;
}

}  // namespace

// -------------------------------------------------------------------- JSON ---

TEST(Json, ScalarsRoundTrip) {
  EXPECT_EQ(Json::null().dump(), "null");
  EXPECT_EQ(Json::boolean(true).dump(), "true");
  EXPECT_EQ(Json::boolean(false).dump(), "false");
  // Integral doubles must not grow a decimal point: glTF indices are integers.
  EXPECT_EQ(Json::number(42).dump(), "42");
  EXPECT_EQ(Json::number(-7).dump(), "-7");
  EXPECT_EQ(Json::number(0.5).dump(), "0.5");
  EXPECT_EQ(Json::string("hi").dump(), "\"hi\"");
}

TEST(Json, EscapesControlCharactersAndQuotes) {
  const Json value = Json::string("a\"b\\c\nd\te\x01");
  const std::string text = value.dump();
  EXPECT_NE(text.find("\\\""), std::string::npos);
  EXPECT_NE(text.find("\\\\"), std::string::npos);
  EXPECT_NE(text.find("\\n"), std::string::npos);
  EXPECT_NE(text.find("\\t"), std::string::npos);
  EXPECT_NE(text.find("\\u0001"), std::string::npos);

  Json parsed;
  std::string error;
  ASSERT_TRUE(Json::parse(text, &parsed, &error)) << error;
  EXPECT_EQ(parsed.asString(), value.asString());
}

TEST(Json, ObjectsAndArraysRoundTrip) {
  Json root = Json::object();
  root.set("name", Json::string("parallax"));
  root.set("count", Json::integer(3));
  root.set("ratio", Json::number(0.25));
  root.set("flag", Json::boolean(true));
  Json list = Json::array();
  for (int i = 0; i < 4; ++i) list.push(Json::integer(i * i));
  root.set("squares", std::move(list));
  Json nested = Json::object();
  nested.set("deep", Json::string("value"));
  root.set("nested", std::move(nested));

  Json parsed;
  std::string error;
  ASSERT_TRUE(Json::parse(root.dump(), &parsed, &error)) << error;
  EXPECT_EQ(parsed.get("name").asString(), "parallax");
  EXPECT_EQ(parsed.get("count").asInt(), 3);
  EXPECT_DOUBLE_EQ(parsed.get("ratio").asDouble(), 0.25);
  EXPECT_TRUE(parsed.get("flag").asBool());
  ASSERT_EQ(parsed.get("squares").size(), 4u);
  EXPECT_EQ(parsed.get("squares").at(3).asInt(), 9);
  EXPECT_EQ(parsed.get("nested").get("deep").asString(), "value");

  // Pretty printing must parse back to the same document.
  Json pretty;
  ASSERT_TRUE(Json::parse(root.dump(2), &pretty, &error)) << error;
  EXPECT_EQ(pretty.dump(), parsed.dump());
}

TEST(Json, AccessorsReturnFallbacksForWrongTypes) {
  const Json text = Json::string("not a number");
  EXPECT_EQ(text.asInt(-1), -1);
  EXPECT_DOUBLE_EQ(text.asDouble(2.5), 2.5);
  EXPECT_TRUE(text.asBool(true));
  EXPECT_TRUE(text.get("missing").isNull());
  EXPECT_TRUE(text.at(0).isNull());
  EXPECT_EQ(text.size(), 0u);

  const Json number = Json::number(1);
  EXPECT_TRUE(number.asString().empty());

  Json array = Json::array();
  array.push(Json::integer(5));
  EXPECT_TRUE(array.at(99).isNull());
  EXPECT_FALSE(array.has("key"));
}

TEST(Json, ParsesNumberForms) {
  const char* inputs[] = {"0", "-0", "1e3", "1E+3", "-2.5e-3", "123.456", "0.0"};
  const double expected[] = {0, 0, 1000, 1000, -0.0025, 123.456, 0.0};
  for (size_t i = 0; i < sizeof(expected) / sizeof(double); ++i) {
    Json value;
    std::string error;
    ASSERT_TRUE(Json::parse(inputs[i], &value, &error)) << inputs[i] << ": " << error;
    EXPECT_NEAR(value.asDouble(), expected[i], 1e-12) << inputs[i];
  }
}

TEST(Json, RejectsMalformedInput) {
  const char* bad[] = {
      "",        "{",        "}",        "[1,2",     "{\"a\":}",  "{\"a\" 1}",
      "tru",     "nul",      "\"abc",    "01x",      "{,}",       "[1,2]trailing",
      "{\"a\":1,}",  "1e",   "--1",      "\"\\q\"",
  };
  for (const char* input : bad) {
    Json value;
    std::string error;
    EXPECT_FALSE(Json::parse(input, &value, &error)) << "accepted: " << input;
    EXPECT_FALSE(error.empty()) << input;
  }
}

TEST(Json, RejectsPathologicalNesting) {
  // A deeply nested document must be refused rather than overflowing the stack.
  std::string deep(500, '[');
  Json value;
  std::string error;
  EXPECT_FALSE(Json::parse(deep, &value, &error));
  EXPECT_NE(error.find("deep"), std::string::npos);
}

TEST(Json, NonFiniteNumbersSerialiseAsZeroRatherThanInvalidJson) {
  // JSON has no NaN or Infinity, so emitting them would produce a document no
  // parser accepts. Zero is wrong but recoverable; invalid JSON is not.
  const std::string text = Json::number(std::nan("")).dump();
  Json parsed;
  EXPECT_TRUE(Json::parse(text, &parsed));
  EXPECT_EQ(Json::number(std::numeric_limits<double>::infinity()).dump(), "0");
}

TEST(Json, SettingAValueReplacesTheOldOne) {
  Json object = Json::object();
  object.set("k", Json::integer(1));
  object.set("k", Json::integer(2));
  EXPECT_EQ(object.size(), 1u);
  EXPECT_EQ(object.get("k").asInt(), 2);
}

// --------------------------------------------------------------------- PNG ---

TEST(Png, EncodeDecodeRoundTripsExactly) {
  for (auto size : {std::pair<int, int>{1, 1}, {7, 3}, {64, 64}, {129, 37}}) {
    const ColorBuffer original = gradient(size.first, size.second);
    std::vector<uint8_t> png;
    ASSERT_TRUE(encodePng(original, &png)) << size.first << "x" << size.second;

    // PNG signature.
    ASSERT_GE(png.size(), 8u);
    EXPECT_EQ(png[0], 0x89);
    EXPECT_EQ(png[1], 'P');
    EXPECT_EQ(png[2], 'N');
    EXPECT_EQ(png[3], 'G');

    ColorBuffer decoded;
    ASSERT_TRUE(decodePng(png, &decoded)) << size.first << "x" << size.second;
    ASSERT_EQ(decoded.width, original.width);
    ASSERT_EQ(decoded.height, original.height);
    // PNG is lossless, so this must be bit exact.
    EXPECT_EQ(decoded.pixels, original.pixels);
  }
}

TEST(Png, GreyscaleRoundTripsThroughTheRgbDecoder) {
  GrayBuffer gray(32, 24);
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 32; ++x) gray.at(x, y) = static_cast<uint8_t>((x * 8 + y * 3) & 0xFF);
  }
  std::vector<uint8_t> png;
  ASSERT_TRUE(encodePngGray(gray, &png));

  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(png, &decoded));
  ASSERT_EQ(decoded.width, 32);
  ASSERT_EQ(decoded.height, 24);
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 32; ++x) {
      uint8_t rgb[3];
      decoded.get(x, y, rgb);
      // Greyscale expands to equal channels.
      EXPECT_EQ(rgb[0], gray.at(x, y));
      EXPECT_EQ(rgb[1], gray.at(x, y));
      EXPECT_EQ(rgb[2], gray.at(x, y));
    }
  }
}

TEST(Png, CompressionActuallyCompresses) {
  // A flat image should compress enormously; the row filters plus deflate turn
  // it into almost nothing.
  ColorBuffer flat(256, 256);
  for (auto& p : flat.pixels) p = 77;
  std::vector<uint8_t> png;
  ASSERT_TRUE(encodePng(flat, &png));
  EXPECT_LT(png.size(), flat.pixels.size() / 50);

  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(png, &decoded));
  EXPECT_EQ(decoded.pixels, flat.pixels);
}

TEST(Png, HigherCompressionIsNotLarger) {
  const ColorBuffer image = gradient(128, 128);
  std::vector<uint8_t> fast, slow;
  ASSERT_TRUE(encodePng(image, &fast, 1));
  ASSERT_TRUE(encodePng(image, &slow, 9));
  EXPECT_LE(slow.size(), fast.size());
  ColorBuffer a, b;
  ASSERT_TRUE(decodePng(fast, &a));
  ASSERT_TRUE(decodePng(slow, &b));
  EXPECT_EQ(a.pixels, b.pixels);
}

TEST(Png, RejectsEmptyAndCorruptInput) {
  std::vector<uint8_t> png;
  EXPECT_FALSE(encodePng(ColorBuffer{}, &png));
  EXPECT_FALSE(encodePngGray(GrayBuffer{}, &png));

  ColorBuffer decoded;
  EXPECT_FALSE(decodePng({}, &decoded));
  EXPECT_FALSE(decodePng(std::vector<uint8_t>(64, 0), &decoded));

  // Valid signature, then nonsense.
  ASSERT_TRUE(encodePng(gradient(16, 16), &png));
  std::vector<uint8_t> broken = png;
  broken[20] ^= 0xFF;  // corrupt inside IHDR
  EXPECT_FALSE(decodePng(broken, &decoded));

  std::vector<uint8_t> truncated(png.begin(), png.begin() + png.size() / 2);
  EXPECT_FALSE(decodePng(truncated, &decoded));
}

TEST(Png, FileRoundTrip) {
  TempDir dir;
  const ColorBuffer original = gradient(48, 32);
  const std::string path = dir.file("image.png");
  ASSERT_TRUE(writePngFile(path, original));

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(readFileBytes(path, &bytes));
  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(bytes, &decoded));
  EXPECT_EQ(decoded.pixels, original.pixels);
}

TEST(Io, FileHelpersRoundTripAndFailCleanly) {
  TempDir dir;
  const std::string path = dir.file("text.txt");
  ASSERT_TRUE(writeFileText(path, "hello\nworld"));
  std::string text;
  ASSERT_TRUE(readFileText(path, &text));
  EXPECT_EQ(text, "hello\nworld");

  const std::vector<uint8_t> bytes{0, 1, 2, 255, 128};
  const std::string bin = dir.file("data.bin");
  ASSERT_TRUE(writeFileBytes(bin, bytes));
  std::vector<uint8_t> loaded;
  ASSERT_TRUE(readFileBytes(bin, &loaded));
  EXPECT_EQ(loaded, bytes);

  // An empty file is valid and must round-trip.
  const std::string empty = dir.file("empty.bin");
  ASSERT_TRUE(writeFileBytes(empty, {}));
  ASSERT_TRUE(readFileBytes(empty, &loaded));
  EXPECT_TRUE(loaded.empty());

  EXPECT_FALSE(readFileBytes(dir.file("nope.bin"), &loaded));
  EXPECT_FALSE(readFileText(dir.file("nope.txt"), &text));
}

TEST(Io, ToGrayUsesRec601Luma) {
  ColorBuffer image(3, 1);
  image.set(0, 0, 255, 0, 0);
  image.set(1, 0, 0, 255, 0);
  image.set(2, 0, 0, 0, 255);
  const GrayBuffer gray = toGray(image);
  EXPECT_EQ(gray.at(0, 0), static_cast<uint8_t>(std::lround(0.299 * 255)));
  EXPECT_EQ(gray.at(1, 0), static_cast<uint8_t>(std::lround(0.587 * 255)));
  EXPECT_EQ(gray.at(2, 0), static_cast<uint8_t>(std::lround(0.114 * 255)));
}

// --------------------------------------------------------------------- ZIP ---

TEST(Zip, RoundTripsStoredAndDeflatedEntries) {
  ZipWriter writer;
  const std::string text(5000, 'a');  // highly compressible
  const std::vector<uint8_t> binary{1, 2, 3, 4, 5};
  ASSERT_TRUE(writer.addText("doc.txt", text, ZipCompression::Deflate));
  ASSERT_TRUE(writer.add("raw.bin", binary, ZipCompression::Store));
  ASSERT_TRUE(writer.addText("nested/dir/file.json", "{\"a\":1}", ZipCompression::Deflate));
  EXPECT_EQ(writer.entryCount(), 3u);
  const std::vector<uint8_t> archive = writer.finish();

  std::vector<std::string> names;
  ASSERT_TRUE(zipList(archive, &names));
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "doc.txt");
  EXPECT_EQ(names[2], "nested/dir/file.json");

  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(archive, &entries));
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(std::string(entries[0].data.begin(), entries[0].data.end()), text);
  EXPECT_EQ(entries[1].data, binary);
  EXPECT_EQ(std::string(entries[2].data.begin(), entries[2].data.end()), "{\"a\":1}");

  // Deflate must have actually shrunk the compressible entry.
  EXPECT_LT(archive.size(), text.size());
}

TEST(Zip, AlignmentPadsEntryDataAsUsdzRequires) {
  ZipWriter writer(64);
  ASSERT_TRUE(writer.addText("first.usda", "#usda 1.0\n", ZipCompression::Store));
  ASSERT_TRUE(writer.add("textures/a.png", std::vector<uint8_t>(300, 9),
                         ZipCompression::Store));
  ASSERT_TRUE(writer.addText("b.txt", "x", ZipCompression::Store));
  const std::vector<uint8_t> archive = writer.finish();

  for (const char* name : {"first.usda", "textures/a.png", "b.txt"}) {
    const int64_t offset = zipEntryDataOffset(archive, name);
    ASSERT_GE(offset, 0) << name;
    EXPECT_EQ(offset % 64, 0) << name;
  }
  // Contents must survive the padding.
  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(archive, &entries));
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[1].data.size(), 300u);
}

TEST(Zip, DefaultAlignmentDoesNotPad) {
  ZipWriter writer;  // alignment 1
  ASSERT_TRUE(writer.addText("a.txt", "hello", ZipCompression::Store));
  const std::vector<uint8_t> archive = writer.finish();
  // 30-byte local header plus the 5-character name, with no extra field.
  EXPECT_EQ(zipEntryDataOffset(archive, "a.txt"), 35);
}

TEST(Zip, AlreadyCompressedDataFallsBackToStore) {
  // Random bytes cannot be deflated smaller; the writer must notice and store
  // them rather than growing the archive.
  Rng rng(3);
  std::vector<uint8_t> noise(4096);
  for (uint8_t& b : noise) b = static_cast<uint8_t>(rng.next() & 0xFF);

  ZipWriter writer;
  ASSERT_TRUE(writer.add("noise.bin", noise, ZipCompression::Deflate));
  const std::vector<uint8_t> archive = writer.finish();
  // Allow for headers, but the payload itself must not have grown.
  EXPECT_LT(archive.size(), noise.size() + 256);

  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(archive, &entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].data, noise);
}

TEST(Zip, RejectsUnsafePaths) {
  ZipWriter writer;
  EXPECT_FALSE(writer.addText("/absolute.txt", "x"));
  EXPECT_FALSE(writer.addText("../escape.txt", "x"));
  EXPECT_FALSE(writer.addText("a/../../b.txt", "x"));
  EXPECT_FALSE(writer.addText("windows\\path.txt", "x"));
  EXPECT_FALSE(writer.addText("", "x"));
  EXPECT_EQ(writer.entryCount(), 0u);
}

TEST(Zip, ReaderRejectsCorruptArchives) {
  std::vector<ZipEntry> entries;
  std::vector<std::string> names;
  EXPECT_FALSE(zipRead({}, &entries));
  EXPECT_FALSE(zipList(std::vector<uint8_t>(100, 0), &names));

  ZipWriter writer;
  ASSERT_TRUE(writer.addText("a.txt", std::string(1000, 'z')));
  std::vector<uint8_t> archive = writer.finish();

  // Truncating removes the end-of-central-directory record.
  std::vector<uint8_t> truncated(archive.begin(), archive.begin() + archive.size() / 2);
  EXPECT_FALSE(zipList(truncated, &names));

  EXPECT_EQ(zipEntryDataOffset(archive, "missing.txt"), -1);
}

TEST(Zip, EmptyEntriesAndEmptyArchivesAreValid) {
  ZipWriter writer;
  ASSERT_TRUE(writer.add("empty.bin", {}, ZipCompression::Deflate));
  const std::vector<uint8_t> archive = writer.finish();
  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(archive, &entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_TRUE(entries[0].data.empty());

  ZipWriter empty_writer;
  const std::vector<uint8_t> empty_archive = empty_writer.finish();
  std::vector<std::string> names;
  ASSERT_TRUE(zipList(empty_archive, &names));
  EXPECT_TRUE(names.empty());
}

TEST(Zip, LargeArchiveWithManyEntries) {
  ZipWriter writer;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(writer.addText("file" + std::to_string(i) + ".txt",
                               std::string(100 + i, static_cast<char>('a' + i % 26))));
  }
  const std::vector<uint8_t> archive = writer.finish();
  std::vector<ZipEntry> entries;
  ASSERT_TRUE(zipRead(archive, &entries));
  ASSERT_EQ(entries.size(), 200u);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(entries[i].path, "file" + std::to_string(i) + ".txt");
    EXPECT_EQ(entries[i].data.size(), static_cast<size_t>(100 + i));
  }
}
