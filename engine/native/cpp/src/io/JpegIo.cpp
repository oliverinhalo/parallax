#include "parallax/io/JpegIo.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace parallax {
namespace {

// ---- markers ---------------------------------------------------------------

constexpr uint8_t kMarkerPrefix = 0xFF;
constexpr uint8_t kSoi = 0xD8;
constexpr uint8_t kEoi = 0xD9;
constexpr uint8_t kSof0 = 0xC0;  // baseline
constexpr uint8_t kSof1 = 0xC1;  // extended sequential, decodes identically
constexpr uint8_t kSof2 = 0xC2;  // progressive
constexpr uint8_t kDht = 0xC4;
constexpr uint8_t kSos = 0xDA;
constexpr uint8_t kDqt = 0xDB;
constexpr uint8_t kDri = 0xDD;

constexpr int kBlockSize = 8;
constexpr int kBlockPixels = kBlockSize * kBlockSize;
constexpr int kMaxComponents = 4;

/// Zig-zag order: coefficients are stored along the diagonals, so index i of the
/// stream belongs at kZigZag[i] of the 8x8 block.
constexpr int kZigZag[kBlockPixels] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/**
 * One Huffman table, as a canonical code built from the bit-length counts.
 *
 * Decoding walks one bit at a time comparing against the first code of each
 * length. That is slower than a lookup table but it is a direct transcription of
 * the spec's procedure, and the cost is irrelevant next to the IDCT.
 */
struct HuffmanTable {
  bool present = false;
  std::array<int, 17> min_code{};
  std::array<int, 17> max_code{};
  std::array<int, 17> value_index{};
  std::vector<uint8_t> values;

  void build(const uint8_t counts[16], const std::vector<uint8_t>& table_values) {
    values = table_values;
    int code = 0;
    int index = 0;
    for (int length = 1; length <= 16; ++length) {
      const int count = counts[length - 1];
      value_index[length] = index;
      min_code[length] = code;
      // max_code is exclusive-of-nothing: -1 marks a length with no codes, which
      // the decode loop treats as "cannot match here".
      max_code[length] = count > 0 ? code + count - 1 : -1;
      code += count;
      index += count;
      code <<= 1;
    }
    present = index > 0;
  }
};

struct Component {
  int id = 0;
  int horizontal_sampling = 1;
  int vertical_sampling = 1;
  int quant_table = 0;
  int dc_table = 0;
  int ac_table = 0;
  int dc_predictor = 0;
  /// Per-component plane at its own (subsampled) resolution.
  std::vector<uint8_t> plane;
  int plane_width = 0;
  int plane_height = 0;
};

/// Reads bits out of the entropy-coded segment, undoing byte stuffing.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t position() const { return position_; }

  /// Next bit, or -1 at the end of the segment.
  int bit() {
    if (bits_left_ == 0) {
      if (position_ >= size_) return -1;
      uint8_t byte = data_[position_++];
      if (byte == kMarkerPrefix) {
        // 0xFF00 is a stuffed 0xFF. Anything else is a marker, which ends the
        // segment: reading past it would decode the marker as image data.
        if (position_ >= size_) return -1;
        const uint8_t next = data_[position_];
        if (next == 0x00) {
          ++position_;
        } else {
          hit_marker_ = true;
          return -1;
        }
      }
      current_ = byte;
      bits_left_ = 8;
    }
    --bits_left_;
    return (current_ >> bits_left_) & 1;
  }

  /// `count` bits, most significant first.
  int bits(int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
      const int next = bit();
      if (next < 0) return value << (count - i - 1);
      value = (value << 1) | next;
    }
    return value;
  }

  /// Discard any partial byte, as a restart marker requires.
  void alignToByte() { bits_left_ = 0; }

  bool hitMarker() const { return hit_marker_; }

  /// Skip an expected RSTn marker. Returns false when one is not there.
  bool skipRestartMarker() {
    alignToByte();
    hit_marker_ = false;
    while (position_ + 1 < size_) {
      if (data_[position_] == kMarkerPrefix) {
        const uint8_t marker = data_[position_ + 1];
        if (marker >= 0xD0 && marker <= 0xD7) {
          position_ += 2;
          return true;
        }
        return false;
      }
      ++position_;
    }
    return false;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
  uint8_t current_ = 0;
  int bits_left_ = 0;
  bool hit_marker_ = false;
};

int decodeHuffman(BitReader& reader, const HuffmanTable& table) {
  int code = 0;
  for (int length = 1; length <= 16; ++length) {
    const int next = reader.bit();
    if (next < 0) return -1;
    code = (code << 1) | next;
    if (table.max_code[length] >= 0 && code <= table.max_code[length]) {
      const int index = table.value_index[length] + (code - table.min_code[length]);
      if (index < 0 || index >= static_cast<int>(table.values.size())) return -1;
      return table.values[index];
    }
  }
  return -1;
}

/**
 * Sign-extend a JPEG magnitude.
 *
 * Coefficients are coded as a bit length plus that many bits. The spec's
 * convention is that values whose top bit is 0 are negative, offset so that the
 * two halves of the range are contiguous; this is the standard EXTEND procedure.
 */
int extend(int value, int length) {
  if (length == 0) return 0;
  const int threshold = 1 << (length - 1);
  return value < threshold ? value - (1 << length) + 1 : value;
}

/**
 * AAN-style separable 8x8 inverse DCT, in fixed point.
 *
 * Rows then columns, both using the same butterfly. The constants are the usual
 * scaled cosines: 1.082392200 = sqrt(2) * cos(3pi/8) * 2, 1.414213562 = sqrt(2),
 * 1.847759065 = sqrt(2) * cos(pi/8) * 2. Working in 13.3 fixed point keeps the
 * intermediate precision above the 8-bit output's needs while staying in
 * 32-bit integers.
 */
void inverseDct(const int* input, uint8_t* output, int output_stride) {
  constexpr int kPass1Bits = 2;
  constexpr int kConstBits = 13;
  constexpr int kOne = 1 << kConstBits;

  constexpr int kC0_298631336 = 2446;
  constexpr int kC0_390180644 = 3196;
  constexpr int kC0_541196100 = 4433;
  constexpr int kC0_765366865 = 6270;
  constexpr int kC0_899976223 = 7373;
  constexpr int kC1_175875602 = 9633;
  constexpr int kC1_501321110 = 12299;
  constexpr int kC1_847759065 = 15137;
  constexpr int kC1_961570560 = 16069;
  constexpr int kC2_053119869 = 16819;
  constexpr int kC2_562915447 = 20995;
  constexpr int kC3_072711026 = 25172;

  int workspace[kBlockPixels];

  // ---- pass 1: columns ----
  for (int column = 0; column < kBlockSize; ++column) {
    const int* in = input + column;
    int* out = workspace + column;

    // A column of nothing but a DC coefficient is extremely common (flat areas
    // and the chroma planes), and short-circuiting it is the single biggest win
    // available in a scalar IDCT.
    if (in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 && in[40] == 0 &&
        in[48] == 0 && in[56] == 0) {
      const int dc = in[0] << kPass1Bits;
      for (int row = 0; row < kBlockSize; ++row) out[row * 8] = dc;
      continue;
    }

    int z2 = in[16];
    int z3 = in[48];
    int z1 = (z2 + z3) * kC0_541196100;
    int tmp2 = z1 + z3 * -kC1_847759065;
    int tmp3 = z1 + z2 * kC0_765366865;

    z2 = in[0];
    z3 = in[32];
    int tmp0 = (z2 + z3) << kConstBits;
    int tmp1 = (z2 - z3) << kConstBits;

    const int t10 = tmp0 + tmp3;
    const int t13 = tmp0 - tmp3;
    const int t11 = tmp1 + tmp2;
    const int t12 = tmp1 - tmp2;

    tmp0 = in[56];
    tmp1 = in[40];
    tmp2 = in[24];
    tmp3 = in[8];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int z4 = tmp1 + tmp3;
    int z5 = (z3 + z4) * kC1_175875602;

    tmp0 = tmp0 * kC0_298631336;
    tmp1 = tmp1 * kC2_053119869;
    tmp2 = tmp2 * kC3_072711026;
    tmp3 = tmp3 * kC1_501321110;
    z1 = z1 * -kC0_899976223;
    z2 = z2 * -kC2_562915447;
    z3 = z3 * -kC1_961570560;
    z4 = z4 * -kC0_390180644;

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    const int descale = kConstBits - kPass1Bits;
    const int rounding = 1 << (descale - 1);
    out[0] = (t10 + tmp3 + rounding) >> descale;
    out[56] = (t10 - tmp3 + rounding) >> descale;
    out[8] = (t11 + tmp2 + rounding) >> descale;
    out[48] = (t11 - tmp2 + rounding) >> descale;
    out[16] = (t12 + tmp1 + rounding) >> descale;
    out[40] = (t12 - tmp1 + rounding) >> descale;
    out[24] = (t13 + tmp0 + rounding) >> descale;
    out[32] = (t13 - tmp0 + rounding) >> descale;
  }

  // ---- pass 2: rows, with the level shift and clamp ----
  const auto clamp = [](int value) -> uint8_t {
    const int shifted = value + 128;
    if (shifted < 0) return 0;
    if (shifted > 255) return 255;
    return static_cast<uint8_t>(shifted);
  };

  for (int row = 0; row < kBlockSize; ++row) {
    const int* in = workspace + row * 8;
    uint8_t* out = output + row * output_stride;

    int z2 = in[2];
    int z3 = in[6];
    int z1 = (z2 + z3) * kC0_541196100;
    int tmp2 = z1 + z3 * -kC1_847759065;
    int tmp3 = z1 + z2 * kC0_765366865;

    int tmp0 = (in[0] + in[4]) << kConstBits;
    int tmp1 = (in[0] - in[4]) << kConstBits;

    const int t10 = tmp0 + tmp3;
    const int t13 = tmp0 - tmp3;
    const int t11 = tmp1 + tmp2;
    const int t12 = tmp1 - tmp2;

    tmp0 = in[7];
    tmp1 = in[5];
    tmp2 = in[3];
    tmp3 = in[1];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int z4 = tmp1 + tmp3;
    int z5 = (z3 + z4) * kC1_175875602;

    tmp0 = tmp0 * kC0_298631336;
    tmp1 = tmp1 * kC2_053119869;
    tmp2 = tmp2 * kC3_072711026;
    tmp3 = tmp3 * kC1_501321110;
    z1 = z1 * -kC0_899976223;
    z2 = z2 * -kC2_562915447;
    z3 = z3 * -kC1_961570560;
    z4 = z4 * -kC0_390180644;

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    const int descale = kConstBits + kPass1Bits + 3;
    const int rounding = 1 << (descale - 1);
    out[0] = clamp((t10 + tmp3 + rounding) >> descale);
    out[7] = clamp((t10 - tmp3 + rounding) >> descale);
    out[1] = clamp((t11 + tmp2 + rounding) >> descale);
    out[6] = clamp((t11 - tmp2 + rounding) >> descale);
    out[2] = clamp((t12 + tmp1 + rounding) >> descale);
    out[5] = clamp((t12 - tmp1 + rounding) >> descale);
    out[3] = clamp((t13 + tmp0 + rounding) >> descale);
    out[4] = clamp((t13 - tmp0 + rounding) >> descale);
  }
  (void)kOne;
}

struct Decoder {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t cursor = 0;

  int width = 0;
  int height = 0;
  int component_count = 0;
  Component components[kMaxComponents];
  uint16_t quant[4][kBlockPixels]{};
  bool quant_present[4]{};
  HuffmanTable dc_tables[4];
  HuffmanTable ac_tables[4];
  int restart_interval = 0;
  std::string error;

  int max_h = 1;
  int max_v = 1;

  bool fail(const char* reason) {
    if (error.empty()) error = reason;
    return false;
  }

  bool readUint8(uint8_t* out) {
    if (cursor >= size) return false;
    *out = data[cursor++];
    return true;
  }

  bool readUint16(int* out) {
    if (cursor + 1 >= size) return false;
    *out = (data[cursor] << 8) | data[cursor + 1];
    cursor += 2;
    return true;
  }

  /// Advance to the next marker, returning its code.
  bool nextMarker(uint8_t* marker) {
    // Markers may be preceded by any number of fill bytes.
    while (cursor < size && data[cursor] != kMarkerPrefix) ++cursor;
    while (cursor < size && data[cursor] == kMarkerPrefix) ++cursor;
    if (cursor >= size) return false;
    *marker = data[cursor++];
    return true;
  }

  bool parseSof(uint8_t marker) {
    if (marker == kSof2) return fail("progressive JPEG is not supported");
    if (marker != kSof0 && marker != kSof1) return fail("unsupported JPEG encoding");

    int length = 0;
    if (!readUint16(&length)) return fail("truncated frame header");
    uint8_t precision = 0;
    if (!readUint8(&precision)) return fail("truncated frame header");
    if (precision != 8) return fail("only 8-bit JPEG samples are supported");
    if (!readUint16(&height) || !readUint16(&width)) return fail("truncated frame header");
    if (width <= 0 || height <= 0) return fail("invalid JPEG dimensions");
    // 100 megapixels is far beyond any phone camera and well past what the
    // pipeline could hold; refusing here avoids a multi-gigabyte allocation
    // driven by a corrupt header.
    if (static_cast<int64_t>(width) * height > 100'000'000LL) {
      return fail("JPEG dimensions are implausibly large");
    }

    uint8_t components = 0;
    if (!readUint8(&components)) return fail("truncated frame header");
    if (components != 1 && components != 3) {
      return fail("only greyscale and YCbCr JPEG are supported");
    }
    component_count = components;

    for (int i = 0; i < component_count; ++i) {
      uint8_t id = 0, sampling = 0, table = 0;
      if (!readUint8(&id) || !readUint8(&sampling) || !readUint8(&table)) {
        return fail("truncated component spec");
      }
      Component& component = components_at(i);
      component.id = id;
      component.horizontal_sampling = (sampling >> 4) & 0x0F;
      component.vertical_sampling = sampling & 0x0F;
      component.quant_table = table & 0x03;
      if (component.horizontal_sampling < 1 || component.vertical_sampling < 1 ||
          component.horizontal_sampling > 4 || component.vertical_sampling > 4) {
        return fail("unsupported chroma sampling");
      }
      max_h = std::max(max_h, component.horizontal_sampling);
      max_v = std::max(max_v, component.vertical_sampling);
    }
    return true;
  }

  Component& components_at(int index) { return components[index]; }

  bool parseDqt() {
    int length = 0;
    if (!readUint16(&length)) return fail("truncated quantisation table");
    size_t end = cursor + static_cast<size_t>(length) - 2;
    if (end > size) return fail("truncated quantisation table");
    while (cursor < end) {
      uint8_t spec = 0;
      if (!readUint8(&spec)) return fail("truncated quantisation table");
      const int table = spec & 0x0F;
      const int precision = (spec >> 4) & 0x0F;
      if (table >= 4) return fail("invalid quantisation table index");
      for (int i = 0; i < kBlockPixels; ++i) {
        int value = 0;
        if (precision == 0) {
          uint8_t byte = 0;
          if (!readUint8(&byte)) return fail("truncated quantisation table");
          value = byte;
        } else {
          if (!readUint16(&value)) return fail("truncated quantisation table");
        }
        // Stored in natural order so dequantisation can index the block
        // directly rather than going through the zig-zag twice.
        quant[table][kZigZag[i]] = static_cast<uint16_t>(value);
      }
      quant_present[table] = true;
    }
    return true;
  }

  bool parseDht() {
    int length = 0;
    if (!readUint16(&length)) return fail("truncated Huffman table");
    size_t end = cursor + static_cast<size_t>(length) - 2;
    if (end > size) return fail("truncated Huffman table");
    while (cursor < end) {
      uint8_t spec = 0;
      if (!readUint8(&spec)) return fail("truncated Huffman table");
      const int index = spec & 0x0F;
      const int is_ac = (spec >> 4) & 0x0F;
      if (index >= 4 || is_ac > 1) return fail("invalid Huffman table index");

      uint8_t counts[16];
      int total = 0;
      for (int i = 0; i < 16; ++i) {
        if (!readUint8(&counts[i])) return fail("truncated Huffman table");
        total += counts[i];
      }
      if (cursor + static_cast<size_t>(total) > size) return fail("truncated Huffman table");
      std::vector<uint8_t> values(data + cursor, data + cursor + total);
      cursor += total;

      if (is_ac != 0) {
        ac_tables[index].build(counts, values);
      } else {
        dc_tables[index].build(counts, values);
      }
    }
    return true;
  }

  bool parseDri() {
    int length = 0;
    if (!readUint16(&length)) return fail("truncated restart interval");
    if (!readUint16(&restart_interval)) return fail("truncated restart interval");
    return true;
  }

  bool parseSos() {
    int length = 0;
    if (!readUint16(&length)) return fail("truncated scan header");
    uint8_t scan_components = 0;
    if (!readUint8(&scan_components)) return fail("truncated scan header");
    if (scan_components != component_count) {
      // A single scan covering fewer components than the frame is a progressive
      // construct; baseline always codes all of them together.
      return fail("unsupported JPEG scan layout");
    }
    for (int i = 0; i < scan_components; ++i) {
      uint8_t id = 0, tables = 0;
      if (!readUint8(&id) || !readUint8(&tables)) return fail("truncated scan header");
      Component* component = nullptr;
      for (int c = 0; c < component_count; ++c) {
        if (components[c].id == id) component = &components[c];
      }
      if (component == nullptr) return fail("scan names an unknown component");
      component->dc_table = (tables >> 4) & 0x0F;
      component->ac_table = tables & 0x0F;
      if (component->dc_table >= 4 || component->ac_table >= 4) {
        return fail("invalid Huffman table selector");
      }
    }
    // Spectral selection and successive approximation bytes; baseline always
    // has 0, 63, 0, so they are read and checked rather than ignored.
    uint8_t spectral_start = 0, spectral_end = 0, approximation = 0;
    if (!readUint8(&spectral_start) || !readUint8(&spectral_end) ||
        !readUint8(&approximation)) {
      return fail("truncated scan header");
    }
    if (spectral_start != 0 || spectral_end != 63) {
      return fail("progressive JPEG is not supported");
    }
    return true;
  }

  bool allocatePlanes() {
    const int mcu_width = max_h * kBlockSize;
    const int mcu_height = max_v * kBlockSize;
    const int mcus_x = (width + mcu_width - 1) / mcu_width;
    const int mcus_y = (height + mcu_height - 1) / mcu_height;
    for (int i = 0; i < component_count; ++i) {
      Component& component = components[i];
      // Planes are allocated to whole MCUs: the last MCU of a row usually
      // extends past the image, and decoding into a tightly sized plane would
      // write out of bounds on every image whose width is not a multiple of 16.
      component.plane_width = mcus_x * component.horizontal_sampling * kBlockSize;
      component.plane_height = mcus_y * component.vertical_sampling * kBlockSize;
      component.plane.assign(
          static_cast<size_t>(component.plane_width) * component.plane_height, 0);
      if (!quant_present[component.quant_table]) {
        return fail("JPEG references a missing quantisation table");
      }
      if (!dc_tables[component.dc_table].present ||
          !ac_tables[component.ac_table].present) {
        return fail("JPEG references a missing Huffman table");
      }
    }
    return true;
  }

  bool decodeScan() {
    const int mcu_width = max_h * kBlockSize;
    const int mcu_height = max_v * kBlockSize;
    const int mcus_x = (width + mcu_width - 1) / mcu_width;
    const int mcus_y = (height + mcu_height - 1) / mcu_height;

    BitReader reader(data + cursor, size - cursor);
    for (int i = 0; i < component_count; ++i) components[i].dc_predictor = 0;

    int until_restart = restart_interval;
    int coefficients[kBlockPixels];
    uint8_t block[kBlockPixels];

    for (int mcu_y = 0; mcu_y < mcus_y; ++mcu_y) {
      for (int mcu_x = 0; mcu_x < mcus_x; ++mcu_x) {
        if (restart_interval > 0 && until_restart == 0) {
          if (!reader.skipRestartMarker()) return fail("expected a restart marker");
          for (int i = 0; i < component_count; ++i) components[i].dc_predictor = 0;
          until_restart = restart_interval;
        }
        if (restart_interval > 0) --until_restart;

        for (int c = 0; c < component_count; ++c) {
          Component& component = components[c];
          for (int by = 0; by < component.vertical_sampling; ++by) {
            for (int bx = 0; bx < component.horizontal_sampling; ++bx) {
              if (!decodeBlock(reader, component, coefficients)) return false;
              inverseDct(coefficients, block, kBlockSize);

              const int x0 =
                  (mcu_x * component.horizontal_sampling + bx) * kBlockSize;
              const int y0 = (mcu_y * component.vertical_sampling + by) * kBlockSize;
              for (int row = 0; row < kBlockSize; ++row) {
                uint8_t* destination =
                    component.plane.data() +
                    static_cast<size_t>(y0 + row) * component.plane_width + x0;
                std::memcpy(destination, block + row * kBlockSize, kBlockSize);
              }
            }
          }
        }
      }
    }
    return true;
  }

  bool decodeBlock(BitReader& reader, Component& component, int* coefficients) {
    std::memset(coefficients, 0, sizeof(int) * kBlockPixels);
    const HuffmanTable& dc = dc_tables[component.dc_table];
    const HuffmanTable& ac = ac_tables[component.ac_table];
    const uint16_t* table = quant[component.quant_table];

    const int dc_length = decodeHuffman(reader, dc);
    if (dc_length < 0 || dc_length > 16) return fail("corrupt DC coefficient");
    const int difference = extend(reader.bits(dc_length), dc_length);
    // DC is coded as a difference from the previous block of the same
    // component, which is why a lost restart marker turns the rest of the image
    // into coloured bands rather than noise.
    component.dc_predictor += difference;
    coefficients[0] = component.dc_predictor * table[0];

    int index = 1;
    while (index < kBlockPixels) {
      const int symbol = decodeHuffman(reader, ac);
      if (symbol < 0) return fail("corrupt AC coefficient");
      const int run = (symbol >> 4) & 0x0F;
      const int magnitude = symbol & 0x0F;
      if (magnitude == 0) {
        // 0x00 ends the block; 0xF0 is a run of sixteen zeroes.
        if (run != 15) break;
        index += 16;
        continue;
      }
      index += run;
      if (index >= kBlockPixels) return fail("AC run overflows the block");
      const int value = extend(reader.bits(magnitude), magnitude);
      coefficients[kZigZag[index]] = value * table[kZigZag[index]];
      ++index;
    }
    return true;
  }

  /// Upsample each plane to full resolution and convert to RGB.
  void toRgb(ColorBuffer* out) {
    *out = ColorBuffer(width, height);
    const Component& first = components[0];

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        uint8_t* pixel = out->pixels.data() +
                         (static_cast<size_t>(y) * width + x) * 3;
        if (component_count == 1) {
          const uint8_t grey = sample(first, x, y);
          pixel[0] = grey;
          pixel[1] = grey;
          pixel[2] = grey;
          continue;
        }
        const int luma = sample(components[0], x, y);
        const int cb = sample(components[1], x, y) - 128;
        const int cr = sample(components[2], x, y) - 128;
        // JFIF YCbCr to RGB, in fixed point: the usual 1.402 / 0.344 / 0.714 /
        // 1.772 coefficients scaled by 2^16.
        const int r = luma + ((91881 * cr) >> 16);
        const int g = luma - ((22554 * cb + 46802 * cr) >> 16);
        const int b = luma + ((116130 * cb) >> 16);
        pixel[0] = clampByte(r);
        pixel[1] = clampByte(g);
        pixel[2] = clampByte(b);
      }
    }
  }

  /**
   * Sample a component plane at full-resolution (x, y), interpolating.
   *
   * The subtlety is where a chroma sample *sits*. With 4:2:0, chroma sample j
   * covers full-resolution pixels [2j, 2j+2), so its centre is at 2j + 0.5 --
   * not at 2j. Sampling at 2j (plain nearest-neighbour replication) shifts the
   * chroma planes half a pixel up and left, which shows up as colour fringing
   * along every strong edge and as a mean error of several code values against
   * a reference decoder.
   *
   * Accounting for the offset and interpolating linearly is exactly the
   * triangle filter libjpeg calls "fancy upsampling", and it is what the
   * reference-decoder test pins this to. The arithmetic is integer: the
   * continuous coordinate is
   *
   *     u = (x + 0.5) * sampling / max - 0.5
   *
   * which, scaled by `denominator = 2 * max`, is the integer
   * `(2x + 1) * sampling - max`.
   */
  uint8_t sample(const Component& component, int x, int y) const {
    const int u_denominator = 2 * max_h;
    const int v_denominator = 2 * max_v;
    const int u_scaled = (2 * x + 1) * component.horizontal_sampling - max_h;
    const int v_scaled = (2 * y + 1) * component.vertical_sampling - max_v;

    const auto split = [](int scaled, int denominator, int* whole, int* fraction) {
      // Floor division, which differs from C's truncation for the negative
      // coordinates that occur at the left and top edges.
      int quotient = scaled / denominator;
      int remainder = scaled % denominator;
      if (remainder < 0) {
        --quotient;
        remainder += denominator;
      }
      *whole = quotient;
      *fraction = remainder;
    };

    int x0 = 0, y0 = 0, fx = 0, fy = 0;
    split(u_scaled, u_denominator, &x0, &fx);
    split(v_scaled, v_denominator, &y0, &fy);

    const int max_x = component.plane_width - 1;
    const int max_y = component.plane_height - 1;
    const int x1 = std::min(x0 + 1, max_x);
    const int y1 = std::min(y0 + 1, max_y);
    const int cx0 = std::clamp(x0, 0, max_x);
    const int cy0 = std::clamp(y0, 0, max_y);
    const int cx1 = std::clamp(x1, 0, max_x);
    const int cy1 = std::clamp(y1, 0, max_y);

    const uint8_t* plane = component.plane.data();
    const int stride = component.plane_width;
    const int p00 = plane[static_cast<size_t>(cy0) * stride + cx0];
    const int p10 = plane[static_cast<size_t>(cy0) * stride + cx1];
    const int p01 = plane[static_cast<size_t>(cy1) * stride + cx0];
    const int p11 = plane[static_cast<size_t>(cy1) * stride + cx1];

    const int top = p00 * (u_denominator - fx) + p10 * fx;
    const int bottom = p01 * (u_denominator - fx) + p11 * fx;
    const int total = top * (v_denominator - fy) + bottom * fy;
    const int divisor = u_denominator * v_denominator;
    return static_cast<uint8_t>((total + divisor / 2) / divisor);
  }

  static uint8_t clampByte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return static_cast<uint8_t>(value);
  }
};

}  // namespace

bool looksLikeJpeg(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 2 && bytes[0] == kMarkerPrefix && bytes[1] == kSoi;
}

bool readJpegSize(const std::vector<uint8_t>& bytes, int* width, int* height) {
  if (!looksLikeJpeg(bytes)) return false;
  Decoder decoder;
  decoder.data = bytes.data();
  decoder.size = bytes.size();
  decoder.cursor = 2;

  uint8_t marker = 0;
  while (decoder.nextMarker(&marker)) {
    if (marker == kSof0 || marker == kSof1 || marker == kSof2) {
      if (!decoder.parseSof(marker == kSof2 ? kSof0 : marker)) return false;
      if (width != nullptr) *width = decoder.width;
      if (height != nullptr) *height = decoder.height;
      return true;
    }
    if (marker == kSos || marker == kEoi) return false;
    // Any other marker segment carries a length we can skip over.
    int length = 0;
    if (!decoder.readUint16(&length) || length < 2) return false;
    decoder.cursor += static_cast<size_t>(length) - 2;
  }
  return false;
}

bool decodeJpeg(const std::vector<uint8_t>& bytes, ColorBuffer* out, std::string* error) {
  const auto report = [error](const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (out == nullptr) return report("no output buffer");
  if (!looksLikeJpeg(bytes)) return report("not a JPEG");

  Decoder decoder;
  decoder.data = bytes.data();
  decoder.size = bytes.size();
  decoder.cursor = 2;

  bool have_frame = false;
  uint8_t marker = 0;
  while (decoder.nextMarker(&marker)) {
    if (marker == kEoi) break;
    if (marker >= 0xD0 && marker <= 0xD7) continue;  // stray restart marker

    if (marker == kSof0 || marker == kSof1 || marker == kSof2) {
      if (!decoder.parseSof(marker)) return report(decoder.error);
      have_frame = true;
      continue;
    }
    if (marker == kDqt) {
      if (!decoder.parseDqt()) return report(decoder.error);
      continue;
    }
    if (marker == kDht) {
      if (!decoder.parseDht()) return report(decoder.error);
      continue;
    }
    if (marker == kDri) {
      if (!decoder.parseDri()) return report(decoder.error);
      continue;
    }
    if (marker == kSos) {
      if (!have_frame) return report("JPEG scan before its frame header");
      if (!decoder.parseSos()) return report(decoder.error);
      if (!decoder.allocatePlanes()) return report(decoder.error);
      if (!decoder.decodeScan()) return report(decoder.error);
      decoder.toRgb(out);
      return true;
    }

    // Everything else (APPn, COM, and so on) carries a length to skip.
    int length = 0;
    if (!decoder.readUint16(&length) || length < 2) return report("corrupt JPEG segment");
    decoder.cursor += static_cast<size_t>(length) - 2;
    if (decoder.cursor > decoder.size) return report("truncated JPEG segment");
  }
  return report("JPEG ended before any image data");
}

}  // namespace parallax
