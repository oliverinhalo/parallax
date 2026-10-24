// Mesh readers.
//
// These exist primarily so the exporters can be validated by round-trip: write
// a mesh, read it back, and compare geometry, topology and attributes against
// the original. A writer that is only checked for "a file appeared" is not
// checked at all. The PLY and OBJ readers are also what the import path uses
// for user-supplied models.
#pragma once

#include "parallax/core/Types.h"
#include "parallax/io/Json.h"
#include "parallax/mesh/Mesh.h"

#include <string>

namespace parallax {

/// Stanford PLY: ASCII and binary little-endian, with optional normals,
/// colours and uvs. Returns false with a reason on malformed input.
bool readPly(const std::string& path, Mesh* out, std::string* error = nullptr);
bool readPlyBytes(const std::vector<uint8_t>& bytes, Mesh* out, std::string* error = nullptr);

/// PLY point cloud (no faces).
bool readPlyPointCloud(const std::string& path, std::vector<Vec3f>* points,
                       std::vector<Vec3f>* normals,
                       std::vector<std::array<uint8_t, 3>>* colors,
                       std::string* error = nullptr);

/// STL: binary and ASCII. STL is a triangle soup, so the result is welded.
bool readStl(const std::string& path, Mesh* out, std::string* error = nullptr);

/// Wavefront OBJ. Handles v/vt/vn, f with any of the index forms, and
/// triangulates convex polygons by fanning.
bool readObj(const std::string& path, Mesh* out, std::string* error = nullptr);

/// A parsed GLB container.
struct GlbFile {
  Json json;
  std::vector<uint8_t> binary;
};

/// Parse a .glb, checking the container structure (magic, version, declared
/// total length, chunk headers and padding).
bool readGlb(const std::string& path, GlbFile* out, std::string* error = nullptr);

/**
 * Structural validation of a glTF document against the parts of the spec that
 * exporters realistically get wrong: accessor/bufferView bounds, component
 * type and count agreement, required POSITION min/max, index range versus the
 * vertex count, and buffer length agreement.
 *
 * This is not a complete glTF validator, and does not claim to be. It is the
 * subset that catches a malformed file produced by this engine.
 */
bool validateGltf(const Json& document, size_t binary_length, std::string* error);

}  // namespace parallax
