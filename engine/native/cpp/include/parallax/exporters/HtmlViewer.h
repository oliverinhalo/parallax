// A scan as one HTML file that works with no network at all.
//
// The brief asks for a self-contained offline viewer, and "self-contained" is
// taken literally: no CDN, no `<script src>`, no second file. Geometry and
// texture are base64 in the document and the renderer is a few hundred lines
// of WebGL inlined beside them.
//
// It does not embed glTF and a glTF loader. Both ends of this file are written
// here, so the geometry goes in as raw typed arrays and the JavaScript reads
// them straight into buffers -- which is a fraction of the code of a parser
// and has nothing in it that can disagree about the spec.
#pragma once

#include "parallax/exporters/MeshExport.h"
#include "parallax/mesh/Mesh.h"

#include <string>
#include <vector>

namespace parallax {

struct HtmlViewerOptions {
  std::string title = "Parallax scan";
  /// Shown under the title. Measurements, capture date, whatever the app knows.
  std::vector<std::pair<std::string, std::string>> facts;
  /// Longest texture edge in the embedded PNG. Base64 inflates by a third, so
  /// a 4K atlas would make a 20 MB file nobody can email.
  int max_texture_size = 2048;
  /// Decimate before embedding. 0 keeps every triangle.
  int target_triangles = 150000;
  bool dark = true;
};

/// Base64, standard alphabet with padding.
std::string base64Encode(const uint8_t* data, size_t length);
inline std::string base64Encode(const std::vector<uint8_t>& bytes) {
  return base64Encode(bytes.data(), bytes.size());
}

/// The whole document, as a string.
std::string buildHtmlViewer(const Mesh& mesh, const ColorBuffer& texture,
                            const HtmlViewerOptions& options = {});

bool writeHtmlViewer(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
                     const HtmlViewerOptions& options = {});

}  // namespace parallax
