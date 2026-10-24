// PNG encoding and decoding, plus the pixel helpers the texturing stage needs.
//
// PNG rather than JPEG: it is lossless, the format is small enough to implement
// correctly against the spec, and zlib (already a dependency for the ZIP
// containers) supplies the one piece that would otherwise be hard. JPEG would
// save space on photographic textures but needs a DCT codec, and the Android
// layer can transcode via Bitmap when a smaller file matters.
#pragma once

#include "parallax/core/Types.h"

#include <string>

namespace parallax {

/// Encode RGB (3 bytes per pixel) as PNG. `compression` is 0..9 as in zlib.
bool encodePng(const ColorBuffer& image, std::vector<uint8_t>* out, int compression = 6);

/// Encode 8-bit greyscale as PNG.
bool encodePngGray(const GrayBuffer& image, std::vector<uint8_t>* out, int compression = 6);

bool writePngFile(const std::string& path, const ColorBuffer& image, int compression = 6);

/// Decode an 8-bit RGB or greyscale PNG. Supports the subset this engine emits
/// plus the common variants a phone produces: colour types 0, 2 and 6, bit
/// depth 8, no interlacing.
bool decodePng(const std::vector<uint8_t>& bytes, ColorBuffer* out);

/// Read a whole file into memory.
bool readFileBytes(const std::string& path, std::vector<uint8_t>* out);
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes);
bool writeFileText(const std::string& path, const std::string& text);
bool readFileText(const std::string& path, std::string* out);

/// Rec.601 luma conversion, matching Android's YUV_420_888 Y plane.
GrayBuffer toGray(const ColorBuffer& image);

/**
 * Box-filtered downscale to a target width, keeping the aspect ratio.
 *
 * Averaging over the source block rather than picking one pixel from it
 * matters here: splat training compares a render against these images, and a
 * point-sampled downscale keeps the sensor noise at full amplitude while
 * throwing the signal away, so the optimiser spends gaussians fitting grain.
 * Returns the image unchanged when it is already no wider than the target.
 */
ColorBuffer downscaleToWidth(const ColorBuffer& image, int target_width);

}  // namespace parallax
