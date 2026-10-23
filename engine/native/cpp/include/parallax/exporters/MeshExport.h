// Mesh and point-cloud export to every format Parallax offers.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/mesh/Mesh.h"

#include <string>

namespace parallax {

enum class LengthUnit { Metres, Centimetres, Millimetres, Inches, Feet };

/// Multiplier taking metres (the engine's internal unit) to `unit`.
double unitScaleFromMetres(LengthUnit unit);
const char* unitName(LengthUnit unit);

/// Which axis points up in the exported file.
enum class UpAxis {
  YUp,  ///< glTF, USD and most DCC tools
  ZUp,  ///< CAD, Blender's default, most GIS
};

/// Handedness of the exported coordinate system.
enum class Handedness { RightHanded, LeftHanded };

struct ExportOptions {
  LengthUnit unit = LengthUnit::Metres;
  UpAxis up_axis = UpAxis::YUp;
  Handedness handedness = Handedness::RightHanded;
  /// Longest texture edge; the atlas is downscaled to fit. 0 keeps it as is.
  int max_texture_size = 4096;
  /// Decimate to this triangle count before writing. 0 disables.
  int target_triangles = 0;
  /// Write binary rather than ASCII where the format offers both.
  bool binary = true;
  /// Include per-vertex colours when the format supports them.
  bool include_vertex_colors = true;
  bool include_normals = true;
  bool include_uvs = true;
  /// Name embedded in the file (glTF node/mesh name, OBJ group, USD prim).
  std::string name = "ParallaxModel";
  std::string generator = "Parallax 1.0";
};

/**
 * Apply the unit, up-axis and handedness conversions implied by `options`.
 *
 * Done once, up front, by every exporter, so the format writers only ever deal
 * with coordinates already in the destination convention.
 */
Mesh applyExportTransform(const Mesh& mesh, const ExportOptions& options);

/// The 3x3 matrix that `applyExportTransform` applies (useful for transforming
/// annotations and camera poses alongside the geometry).
Mat3 exportBasis(const ExportOptions& options);

// --------------------------------------------------------------- formats ---

/// Stanford PLY. Binary little-endian or ASCII; carries colours and normals.
bool writePly(const std::string& path, const Mesh& mesh, const ExportOptions& options = {});

/// Coloured point cloud as PLY.
bool writePointCloudPly(const std::string& path, const std::vector<Vec3f>& points,
                        const std::vector<Vec3f>& normals,
                        const std::vector<std::array<uint8_t, 3>>& colors,
                        const ExportOptions& options = {});

/**
 * STL. Always binary when `options.binary`, otherwise ASCII.
 *
 * STL has no units field at all, so the caller's unit choice is the only thing
 * that makes the file meaningful -- slicers assume millimetres, which is why
 * the 3D-printing path defaults to them.
 */
bool writeStl(const std::string& path, const Mesh& mesh, const ExportOptions& options = {});

/// OBJ plus a sibling MTL and, when a texture is supplied, a PNG.
bool writeObj(const std::string& directory, const std::string& base_name, const Mesh& mesh,
              const ColorBuffer& texture, const ExportOptions& options = {});

/// Binary glTF 2.0 (.glb), with the texture embedded as a PNG image chunk.
bool writeGlb(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
              const ExportOptions& options = {});

/// glTF 2.0 as separate .gltf + .bin (+ .png) files.
bool writeGltf(const std::string& directory, const std::string& base_name, const Mesh& mesh,
               const ColorBuffer& texture, const ExportOptions& options = {});

/**
 * USDZ: a 64-byte-aligned, uncompressed ZIP containing a USDA stage.
 *
 * Written by hand because no permissively licensed USD implementation is small
 * enough to cross-compile for Android (OpenUSD itself is Apache-2.0 but is a
 * very large dependency). The subset emitted -- a single Mesh prim with points,
 * faceVertexIndices, normals, a texture-coordinate primvar and a UsdPreviewSurface
 * material -- is what Quick Look and every USD importer actually reads.
 */
bool writeUsdz(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
               const ExportOptions& options = {});

/// The USDA text for a mesh, exposed for testing and for the .usda-only export.
std::string buildUsda(const Mesh& mesh, const ExportOptions& options,
                      const std::string& texture_file);

}  // namespace parallax
