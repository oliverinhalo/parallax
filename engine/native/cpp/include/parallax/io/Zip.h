// Minimal ZIP writer and reader.
//
// Two consumers with quite different requirements:
//
//   * `.usdz` is a ZIP archive with hard constraints from the USD spec: every
//     entry must be STORED (no compression) and each entry's *file data* must
//     start at a 64-byte aligned offset, so that a reader can mmap the archive
//     and hand pointers straight to the USD runtime. Violating either makes the
//     package unreadable by USD tooling even though it is a valid ZIP.
//   * `.parallax` project bundles want ordinary DEFLATE to keep backups small.
//
// Both are served here rather than pulling in a ZIP library, because the write
// path is a few hundred lines and the alignment requirement above needs
// explicit control over entry layout that most libraries do not expose.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace parallax {

enum class ZipCompression {
  Store,    ///< no compression; required by USDZ
  Deflate,  ///< standard zlib deflate
};

class ZipWriter {
 public:
  /**
   * @param alignment  byte alignment for each entry's file data. 1 for an
   *                   ordinary archive, 64 for USDZ. Alignment is achieved by
   *                   padding the entry's `extra` field, which is the
   *                   spec-sanctioned mechanism.
   */
  explicit ZipWriter(unsigned alignment = 1);

  /// Add one entry. Paths use forward slashes and must not be absolute.
  bool add(const std::string& path, const std::vector<uint8_t>& data,
           ZipCompression compression = ZipCompression::Deflate);
  bool addText(const std::string& path, const std::string& text,
               ZipCompression compression = ZipCompression::Deflate);

  /// Finish the archive and return its bytes.
  std::vector<uint8_t> finish();

  size_t entryCount() const { return entries_.size(); }

 private:
  struct Entry {
    std::string path;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t method = 0;
    uint32_t local_header_offset = 0;
  };

  unsigned alignment_;
  std::vector<uint8_t> buffer_;
  std::vector<Entry> entries_;
};

struct ZipEntry {
  std::string path;
  std::vector<uint8_t> data;
};

/// Read every entry of an archive. Supports STORED and DEFLATE.
bool zipRead(const std::vector<uint8_t>& archive, std::vector<ZipEntry>* out);

/// Read just the entry names, without inflating anything.
bool zipList(const std::vector<uint8_t>& archive, std::vector<std::string>* out);

/// Byte offset of the named entry's file data, or -1. Used to verify USDZ
/// alignment.
int64_t zipEntryDataOffset(const std::vector<uint8_t>& archive, const std::string& path);

}  // namespace parallax
