// Baseline JPEG decoding.
//
// The capture layer writes frames as JPEG, because that is the one format on
// the path that matters for size: a 160-frame scan is 250 MB of JPEG against
// 1.3 GB of PNG, and the phone's hardware encoder produces it for free from the
// camera's YUV planes. The pipeline therefore has to read it.
//
// Baseline sequential only (SOF0), which is what Android's encoder emits.
// Progressive JPEG (SOF2) is detected and rejected with a clear error rather
// than decoded incorrectly: implementing progressive scans would roughly double
// this file for a format nothing in this app produces. Arithmetic coding,
// 12-bit samples and CMYK are likewise out of scope and reported.
#pragma once

#include "parallax/core/Types.h"

#include <string>

namespace parallax {

/// Decode a baseline JPEG into RGB. `error` receives a reason when decoding
/// fails, which the pipeline surfaces rather than silently producing an empty
/// frame.
bool decodeJpeg(const std::vector<uint8_t>& bytes, ColorBuffer* out,
                std::string* error = nullptr);

/// Read the dimensions without decoding the image data. Used to size buffers
/// and to reject absurd files before allocating for them.
bool readJpegSize(const std::vector<uint8_t>& bytes, int* width, int* height);

/// True when the bytes start with the JPEG SOI marker.
bool looksLikeJpeg(const std::vector<uint8_t>& bytes);

}  // namespace parallax
