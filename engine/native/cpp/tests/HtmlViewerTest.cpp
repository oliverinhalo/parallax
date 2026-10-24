#include "parallax/exporters/HtmlViewer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "parallax/io/ImageIo.h"
#include "parallax/testing/SyntheticScene.h"

using namespace parallax;
using namespace parallax::testing;

namespace {

/// Decode base64 back to bytes, so the round trip can be checked rather than
/// the encoder being compared against itself.
std::vector<uint8_t> base64Decode(const std::string& text) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  int accumulator = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') break;
    const int digit = value(c);
    if (digit < 0) continue;
    accumulator = (accumulator << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
    }
  }
  return out;
}

/// The value of a JSON string field in the embedded model blob.
std::string fieldOf(const std::string& document, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const size_t start = document.find(needle);
  if (start == std::string::npos) return {};
  const size_t from = start + needle.size();
  const size_t end = document.find('"', from);
  return end == std::string::npos ? std::string() : document.substr(from, end - from);
}

Mesh texturedQuad() {
  Mesh mesh;
  mesh.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f),
                   Vec3f(0.f, 1.f, 0.f)};
  mesh.normals = {Vec3f::UnitZ(), Vec3f::UnitZ(), Vec3f::UnitZ(), Vec3f::UnitZ()};
  mesh.uvs = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(1.f, 1.f), Vec2f(0.f, 1.f)};
  mesh.colors = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

}  // namespace

TEST(HtmlViewer, Base64MatchesTheStandardAlphabetAndPadding) {
  // The three canonical padding cases from RFC 4648.
  const auto encode = [](const std::string& text) {
    return base64Encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  };
  EXPECT_EQ("", encode(""));
  EXPECT_EQ("Zg==", encode("f"));
  EXPECT_EQ("Zm8=", encode("fo"));
  EXPECT_EQ("Zm9v", encode("foo"));
  EXPECT_EQ("Zm9vYmFy", encode("foobar"));
}

TEST(HtmlViewer, Base64RoundTripsArbitraryBytes) {
  Rng rng(4242);
  for (size_t length : {1u, 2u, 3u, 4u, 17u, 255u, 1000u}) {
    std::vector<uint8_t> bytes(length);
    for (uint8_t& byte : bytes) byte = static_cast<uint8_t>(rng.uniform(0.0, 256.0));
    const std::vector<uint8_t> decoded = base64Decode(base64Encode(bytes));
    ASSERT_EQ(bytes.size(), decoded.size()) << "length " << length;
    EXPECT_EQ(bytes, decoded) << "length " << length;
  }
}

TEST(HtmlViewer, AnEmptyMeshIsRefusedRatherThanWritingAnEmptyPage) {
  const auto directory = std::filesystem::temp_directory_path() / "parallax_html_empty";
  std::filesystem::create_directories(directory);
  EXPECT_FALSE(writeHtmlViewer((directory / "x.html").string(), Mesh{}, ColorBuffer{}));
  std::filesystem::remove_all(directory);
}

TEST(HtmlViewer, TheDocumentIsSelfContained) {
  const std::string document = buildHtmlViewer(texturedQuad(), ColorBuffer{});

  // No network of any kind: this file has to work in airplane mode, from a
  // messaging app's download folder, on a machine that has never seen it.
  EXPECT_EQ(std::string::npos, document.find("http://"));
  EXPECT_EQ(std::string::npos, document.find("https://"));
  EXPECT_EQ(std::string::npos, document.find("script src"));
  EXPECT_EQ(std::string::npos, document.find("<link"));
  EXPECT_EQ(std::string::npos, document.find("fetch("));
  EXPECT_EQ(std::string::npos, document.find("XMLHttpRequest"));

  EXPECT_NE(std::string::npos, document.find("<!doctype html>"));
  EXPECT_NE(std::string::npos, document.find("</html>"));
  EXPECT_NE(std::string::npos, document.find("getContext('webgl'"));
}

TEST(HtmlViewer, TheGeometryInTheFileIsTheGeometryThatWentIn) {
  const Mesh mesh = texturedQuad();
  HtmlViewerOptions options;
  options.target_triangles = 0;  // no decimation, so the comparison is exact
  const std::string document = buildHtmlViewer(mesh, ColorBuffer{}, options);

  const std::vector<uint8_t> positions = base64Decode(fieldOf(document, "positions"));
  ASSERT_EQ(mesh.vertexCount() * 3 * sizeof(float), positions.size());
  std::vector<float> decoded(mesh.vertexCount() * 3);
  std::memcpy(decoded.data(), positions.data(), positions.size());
  for (size_t v = 0; v < mesh.vertexCount(); ++v) {
    EXPECT_FLOAT_EQ(mesh.vertices[v].x(), decoded[v * 3 + 0]);
    EXPECT_FLOAT_EQ(mesh.vertices[v].y(), decoded[v * 3 + 1]);
    EXPECT_FLOAT_EQ(mesh.vertices[v].z(), decoded[v * 3 + 2]);
  }

  const std::vector<uint8_t> indices = base64Decode(fieldOf(document, "indices"));
  ASSERT_EQ(mesh.indices.size() * sizeof(uint32_t), indices.size());
  std::vector<uint32_t> decoded_indices(mesh.indices.size());
  std::memcpy(decoded_indices.data(), indices.data(), indices.size());
  EXPECT_EQ(mesh.indices, decoded_indices);
}

TEST(HtmlViewer, NormalsUvsAndColoursAreCarriedWhenPresentAndOmittedWhenNot) {
  HtmlViewerOptions options;
  options.target_triangles = 0;
  const std::string full = buildHtmlViewer(texturedQuad(), ColorBuffer{}, options);
  EXPECT_FALSE(fieldOf(full, "normals").empty());
  EXPECT_FALSE(fieldOf(full, "uvs").empty());
  EXPECT_FALSE(fieldOf(full, "colors").empty());

  Mesh bare = texturedQuad();
  bare.uvs.clear();
  bare.colors.clear();
  const std::string plain = buildHtmlViewer(bare, ColorBuffer{}, options);
  EXPECT_TRUE(fieldOf(plain, "uvs").empty());
  EXPECT_TRUE(fieldOf(plain, "colors").empty());
  // Normals are recomputed when they are missing, because the shading needs
  // them and a flat grey model is not worth sending anyone.
  Mesh without_normals = texturedQuad();
  without_normals.normals.clear();
  EXPECT_FALSE(fieldOf(buildHtmlViewer(without_normals, ColorBuffer{}, options), "normals").empty());
}

TEST(HtmlViewer, ATextureIsEmbeddedAsAPngDataSource) {
  ColorBuffer atlas(16, 16);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) atlas.set(x, y, static_cast<uint8_t>(x * 16), 64, 200);
  }
  HtmlViewerOptions options;
  options.target_triangles = 0;
  const std::string document = buildHtmlViewer(texturedQuad(), atlas, options);

  const std::string encoded = fieldOf(document, "texture");
  ASSERT_FALSE(encoded.empty());
  EXPECT_NE(std::string::npos, document.find("data:image/png;base64,"));

  // It really is a PNG, and it really is that image.
  const std::vector<uint8_t> png = base64Decode(encoded);
  ColorBuffer decoded;
  ASSERT_TRUE(decodePng(png, &decoded));
  EXPECT_EQ(16, decoded.width);
  EXPECT_EQ(16, decoded.height);
  uint8_t pixel[3];
  decoded.get(4, 4, pixel);
  EXPECT_EQ(64, pixel[0]);
  EXPECT_EQ(64, pixel[1]);
  EXPECT_EQ(200, pixel[2]);
}

TEST(HtmlViewer, AMeshWithNoUvsGetsNoTextureEvenIfOneIsOffered) {
  Mesh mesh = texturedQuad();
  mesh.uvs.clear();
  ColorBuffer atlas(8, 8);
  HtmlViewerOptions options;
  options.target_triangles = 0;
  // Embedding a megabyte of atlas that nothing can sample is pure waste.
  EXPECT_TRUE(fieldOf(buildHtmlViewer(mesh, atlas, options), "texture").empty());
}

TEST(HtmlViewer, TheTitleAndFactsAreEscapedRatherThanInjected) {
  HtmlViewerOptions options;
  options.title = "Jo's <script>alert(1)</script> \"kitchen\"";
  options.facts = {{"Note & detail", "<b>bold</b>"}};
  const std::string document = buildHtmlViewer(texturedQuad(), ColorBuffer{}, options);

  // A scan name is user input and ends up in a file other people open.
  EXPECT_EQ(std::string::npos, document.find("<script>alert(1)</script>"));
  EXPECT_NE(std::string::npos, document.find("&lt;script&gt;"));
  EXPECT_NE(std::string::npos, document.find("Jo&#39;s"));
  EXPECT_NE(std::string::npos, document.find("Note &amp; detail"));
  EXPECT_NE(std::string::npos, document.find("&lt;b&gt;bold&lt;/b&gt;"));
}

TEST(HtmlViewer, TheCameraIsFramedOnTheModelsOwnBounds) {
  Mesh mesh = texturedQuad();
  for (Vec3f& vertex : mesh.vertices) vertex += Vec3f(10.f, 20.f, 30.f);
  const std::string document = buildHtmlViewer(mesh, ColorBuffer{});

  const size_t centre = document.find("\"center\":[");
  ASSERT_NE(std::string::npos, centre);
  const std::string tail = document.substr(centre + 10, 40);
  // The quad now sits around (10.5, 20.5, 30), so the viewer must open there
  // rather than at the origin, looking at nothing.
  EXPECT_EQ(0u, tail.find("10.5"));
  EXPECT_NE(std::string::npos, tail.find("20.5"));
}

TEST(HtmlViewer, ABigMeshIsDecimatedSoTheFileStaysSendable) {
  const SyntheticScene scene = SyntheticScene::room(4.0, 3.0, 2.4);
  Mesh mesh;
  // A grid dense enough to need decimating, built as a simple height field.
  const int side = 220;
  for (int z = 0; z < side; ++z) {
    for (int x = 0; x < side; ++x) {
      mesh.vertices.emplace_back(x * 0.01f, 0.01f * std::sin(0.3f * x) * std::cos(0.2f * z),
                                 z * 0.01f);
    }
  }
  for (int z = 0; z + 1 < side; ++z) {
    for (int x = 0; x + 1 < side; ++x) {
      const uint32_t a = static_cast<uint32_t>(z * side + x);
      mesh.indices.insert(mesh.indices.end(),
                          {a, a + 1, a + static_cast<uint32_t>(side)});
      mesh.indices.insert(mesh.indices.end(),
                          {a + 1, a + static_cast<uint32_t>(side) + 1,
                           a + static_cast<uint32_t>(side)});
    }
  }
  ASSERT_GT(mesh.triangleCount(), 90000u);

  HtmlViewerOptions options;
  options.target_triangles = 20000;
  const std::string document = buildHtmlViewer(mesh, ColorBuffer{}, options);
  const size_t marker = document.find("\"triangles\":");
  ASSERT_NE(std::string::npos, marker);
  const long written = std::strtol(document.c_str() + marker + 12, nullptr, 10);
  EXPECT_LE(written, 24000);
  EXPECT_GT(written, 1000);
}

TEST(HtmlViewer, TheWrittenFileIsTheDocument) {
  const auto directory = std::filesystem::temp_directory_path() / "parallax_html_write";
  std::filesystem::create_directories(directory);
  const auto path = directory / "scan.html";

  HtmlViewerOptions options;
  options.title = "Bench";
  ASSERT_TRUE(writeHtmlViewer(path.string(), texturedQuad(), ColorBuffer{}, options));

  std::ifstream file(path, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(buildHtmlViewer(texturedQuad(), ColorBuffer{}, options), contents);
  EXPECT_NE(std::string::npos, contents.find("<title>Bench</title>"));
  std::filesystem::remove_all(directory);
}
