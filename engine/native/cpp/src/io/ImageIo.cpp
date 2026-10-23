#include "parallax/io/ImageIo.h"

#include <zlib.h>

#include <cstring>
#include <fstream>

namespace parallax {
namespace {

void appendBigEndian32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(value & 0xFF));
}

uint32_t readBigEndian32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// Append a PNG chunk: length, type, data, CRC32 of (type + data).
void appendChunk(std::vector<uint8_t>* out, const char type[4], const std::vector<uint8_t>& data) {
  appendBigEndian32(out, static_cast<uint32_t>(data.size()));
  const size_t crc_start = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), data.begin(), data.end());
  const uLong crc = crc32(crc32(0L, Z_NULL, 0), out->data() + crc_start,
                          static_cast<uInt>(out->size() - crc_start));
  appendBigEndian32(out, static_cast<uint32_t>(crc));
}

/// Deflate with a zlib wrapper, as PNG's IDAT requires.
bool deflateBytes(const std::vector<uint8_t>& in, std::vector<uint8_t>* out, int level) {
  uLongf bound = compressBound(static_cast<uLong>(in.size()));
  out->resize(bound);
  const int rc = compress2(out->data(), &bound, in.data(), static_cast<uLong>(in.size()),
                           std::max(0, std::min(9, level)));
  if (rc != Z_OK) return false;
  out->resize(bound);
  return true;
}

/// PNG's Paeth predictor, used by filter type 4.
inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  const int p = static_cast<int>(a) + b - c;
  const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

/// Build the raw scanline stream with per-row filtering.
std::vector<uint8_t> buildFilteredRows(const uint8_t* pixels, int width, int height,
                                       int channels) {
  const size_t stride = static_cast<size_t>(width) * channels;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * height);
  std::vector<uint8_t> previous(stride, 0);

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = pixels + static_cast<size_t>(y) * stride;
    // Try the filters PNG defines and keep the one with the smallest sum of
    // absolute differences -- the heuristic libpng uses. Filtering before
    // deflate is where most of PNG's ratio comes from, so it is worth the
    // five passes over a row.
    std::vector<uint8_t> best;
    uint8_t best_type = 0;
    long best_score = -1;

    for (uint8_t filter = 0; filter <= 4; ++filter) {
      std::vector<uint8_t> candidate(stride);
      for (size_t x = 0; x < stride; ++x) {
        const uint8_t left = x >= static_cast<size_t>(channels) ? row[x - channels] : 0;
        const uint8_t up = previous[x];
        const uint8_t up_left =
            x >= static_cast<size_t>(channels) ? previous[x - channels] : 0;
        switch (filter) {
          case 0: candidate[x] = row[x]; break;
          case 1: candidate[x] = static_cast<uint8_t>(row[x] - left); break;
          case 2: candidate[x] = static_cast<uint8_t>(row[x] - up); break;
          case 3:
            candidate[x] =
                static_cast<uint8_t>(row[x] - ((static_cast<int>(left) + up) >> 1));
            break;
          default:
            candidate[x] = static_cast<uint8_t>(row[x] - paeth(left, up, up_left));
            break;
        }
      }
      long score = 0;
      for (uint8_t v : candidate) {
        score += v < 128 ? v : 256 - v;  // signed magnitude
      }
      if (best_score < 0 || score < best_score) {
        best_score = score;
        best = std::move(candidate);
        best_type = filter;
      }
    }
    raw.push_back(best_type);
    raw.insert(raw.end(), best.begin(), best.end());
    previous.assign(row, row + stride);
  }
  return raw;
}

bool encodePngInternal(const uint8_t* pixels, int width, int height, int channels,
                       std::vector<uint8_t>* out, int compression) {
  if (pixels == nullptr || width <= 0 || height <= 0) return false;
  if (channels != 1 && channels != 3) return false;

  out->clear();
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out->insert(out->end(), kSignature, kSignature + 8);

  std::vector<uint8_t> ihdr;
  appendBigEndian32(&ihdr, static_cast<uint32_t>(width));
  appendBigEndian32(&ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);                                    // bit depth
  ihdr.push_back(channels == 1 ? 0 : 2);                // colour type: grey / truecolour
  ihdr.push_back(0);                                    // deflate
  ihdr.push_back(0);                                    // adaptive filtering
  ihdr.push_back(0);                                    // no interlace
  appendChunk(out, "IHDR", ihdr);

  const std::vector<uint8_t> raw = buildFilteredRows(pixels, width, height, channels);
  std::vector<uint8_t> compressed;
  if (!deflateBytes(raw, &compressed, compression)) return false;
  appendChunk(out, "IDAT", compressed);
  appendChunk(out, "IEND", {});
  return true;
}

}  // namespace

bool encodePng(const ColorBuffer& image, std::vector<uint8_t>* out, int compression) {
  if (image.empty()) return false;
  return encodePngInternal(image.pixels.data(), image.width, image.height, 3, out, compression);
}

bool encodePngGray(const GrayBuffer& image, std::vector<uint8_t>* out, int compression) {
  if (image.pixels.empty()) return false;
  return encodePngInternal(image.pixels.data(), image.width, image.height, 1, out, compression);
}

bool writePngFile(const std::string& path, const ColorBuffer& image, int compression) {
  std::vector<uint8_t> bytes;
  if (!encodePng(image, &bytes, compression)) return false;
  return writeFileBytes(path, bytes);
}

bool decodePng(const std::vector<uint8_t>& bytes, ColorBuffer* out) {
  if (bytes.size() < 8 + 25) return false;
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(bytes.data(), kSignature, 8) != 0) return false;

  int width = 0, height = 0, channels = 0;
  std::vector<uint8_t> idat;
  size_t pos = 8;
  bool seen_ihdr = false;

  while (pos + 12 <= bytes.size()) {
    const uint32_t length = readBigEndian32(bytes.data() + pos);
    // Reject a declared length that runs past the buffer before using it.
    if (length > bytes.size() || pos + 12 + length > bytes.size()) return false;
    const char* type = reinterpret_cast<const char*>(bytes.data() + pos + 4);
    const uint8_t* data = bytes.data() + pos + 8;

    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (length < 13) return false;
      width = static_cast<int>(readBigEndian32(data));
      height = static_cast<int>(readBigEndian32(data + 4));
      const uint8_t bit_depth = data[8];
      const uint8_t colour_type = data[9];
      const uint8_t interlace = data[12];
      if (width <= 0 || height <= 0 || bit_depth != 8 || interlace != 0) return false;
      if (colour_type == 0) {
        channels = 1;
      } else if (colour_type == 2) {
        channels = 3;
      } else if (colour_type == 6) {
        channels = 4;
      } else {
        return false;  // palette and grey+alpha are not emitted or consumed here
      }
      // Guard against a decompression bomb before allocating.
      if (static_cast<double>(width) * height > 2e8) return false;
      seen_ihdr = true;
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + length);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    pos += 12 + length;
  }
  if (!seen_ihdr || idat.empty()) return false;

  const size_t stride = static_cast<size_t>(width) * channels;
  std::vector<uint8_t> raw((stride + 1) * height);
  uLongf raw_size = static_cast<uLongf>(raw.size());
  if (uncompress(raw.data(), &raw_size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK) {
    return false;
  }
  if (raw_size != raw.size()) return false;

  // Undo the per-row filtering.
  std::vector<uint8_t> image(stride * height);
  for (int y = 0; y < height; ++y) {
    const uint8_t filter = raw[(stride + 1) * y];
    const uint8_t* src = raw.data() + (stride + 1) * y + 1;
    uint8_t* dst = image.data() + stride * y;
    const uint8_t* up = y > 0 ? image.data() + stride * (y - 1) : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      const uint8_t left = x >= static_cast<size_t>(channels) ? dst[x - channels] : 0;
      const uint8_t above = up ? up[x] : 0;
      const uint8_t above_left =
          (up && x >= static_cast<size_t>(channels)) ? up[x - channels] : 0;
      switch (filter) {
        case 0: dst[x] = src[x]; break;
        case 1: dst[x] = static_cast<uint8_t>(src[x] + left); break;
        case 2: dst[x] = static_cast<uint8_t>(src[x] + above); break;
        case 3:
          dst[x] = static_cast<uint8_t>(src[x] + ((static_cast<int>(left) + above) >> 1));
          break;
        case 4: dst[x] = static_cast<uint8_t>(src[x] + paeth(left, above, above_left)); break;
        default: return false;
      }
    }
  }

  *out = ColorBuffer(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = image.data() + stride * y + static_cast<size_t>(x) * channels;
      if (channels == 1) {
        out->set(x, y, p[0], p[0], p[0]);
      } else {
        out->set(x, y, p[0], p[1], p[2]);  // alpha, if present, is dropped
      }
    }
  }
  return true;
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const std::streamsize size = file.tellg();
  if (size < 0) return false;
  file.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  if (size > 0 && !file.read(reinterpret_cast<char*>(out->data()), size)) return false;
  return true;
}

bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  if (!bytes.empty()) {
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  return file.good();
}

bool writeFileText(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

bool readFileText(const std::string& path, std::string* out) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return false;
  out->assign(bytes.begin(), bytes.end());
  return true;
}

GrayBuffer toGray(const ColorBuffer& image) {
  GrayBuffer gray(image.width, image.height);
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      uint8_t rgb[3];
      image.get(x, y, rgb);
      gray.at(x, y) = static_cast<uint8_t>(
          std::lround(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]));
    }
  }
  return gray;
}

}  // namespace parallax
