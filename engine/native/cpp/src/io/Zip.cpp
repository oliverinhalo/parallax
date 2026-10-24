#include "parallax/io/Zip.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace parallax {
namespace {

constexpr uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50;
constexpr uint32_t kEndOfCentralDirSignature = 0x06054b50;
constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kMethodDeflate = 8;
/// Version 2.0 is the minimum that implies DEFLATE support.
constexpr uint16_t kVersionNeeded = 20;

void put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// Raw deflate (no zlib or gzip wrapper), as ZIP method 8 requires.
bool rawDeflate(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream stream{};
  // windowBits negated selects a raw stream; this is the documented zlib idiom
  // and is exactly what ZIP's method 8 expects.
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  out->resize(deflateBound(&stream, static_cast<uLong>(in.size())) + 32);
  stream.next_in = const_cast<Bytef*>(in.data());
  stream.avail_in = static_cast<uInt>(in.size());
  stream.next_out = out->data();
  stream.avail_out = static_cast<uInt>(out->size());
  const int rc = deflate(&stream, Z_FINISH);
  const size_t written = out->size() - stream.avail_out;
  deflateEnd(&stream);
  if (rc != Z_STREAM_END) return false;
  out->resize(written);
  return true;
}

bool rawInflate(const uint8_t* data, size_t size, size_t expected,
                std::vector<uint8_t>* out) {
  out->assign(expected, 0);
  if (expected == 0) return true;
  z_stream stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  stream.next_out = out->data();
  stream.avail_out = static_cast<uInt>(expected);
  const int rc = inflate(&stream, Z_FINISH);
  const size_t written = expected - stream.avail_out;
  inflateEnd(&stream);
  if (rc != Z_STREAM_END || written != expected) return false;
  return true;
}

bool pathIsSafe(const std::string& path) {
  if (path.empty() || path.size() > 4096) return false;
  if (path.front() == '/' || path.front() == '\\') return false;
  // Reject traversal: a malicious bundle must not be able to write outside the
  // extraction directory.
  if (path.find("..") != std::string::npos) return false;
  if (path.find('\\') != std::string::npos) return false;
  return true;
}

}  // namespace

ZipWriter::ZipWriter(unsigned alignment) : alignment_(std::max(1u, alignment)) {}

bool ZipWriter::add(const std::string& path, const std::vector<uint8_t>& data,
                    ZipCompression compression) {
  if (!pathIsSafe(path)) return false;
  if (data.size() > 0xFFFFFFFFull) return false;  // ZIP64 is not implemented

  std::vector<uint8_t> payload;
  uint16_t method = kMethodStore;
  if (compression == ZipCompression::Deflate && !data.empty()) {
    if (rawDeflate(data, &payload) && payload.size() < data.size()) {
      method = kMethodDeflate;
    } else {
      // Deflate failed or made it bigger (already-compressed content): store.
      payload = data;
    }
  } else {
    payload = data;
  }

  Entry entry;
  entry.path = path;
  entry.crc = static_cast<uint32_t>(
      crc32(crc32(0L, Z_NULL, 0), data.data(), static_cast<uInt>(data.size())));
  entry.uncompressed_size = static_cast<uint32_t>(data.size());
  entry.compressed_size = static_cast<uint32_t>(payload.size());
  entry.method = method;
  entry.local_header_offset = static_cast<uint32_t>(buffer_.size());

  // The local header is 30 bytes plus the name plus the extra field. Pad the
  // extra field so that the file data itself lands on an aligned offset.
  const size_t header_size = 30 + path.size();
  uint16_t extra_length = 0;
  if (alignment_ > 1) {
    const size_t unaligned = (buffer_.size() + header_size) % alignment_;
    if (unaligned != 0) extra_length = static_cast<uint16_t>(alignment_ - unaligned);
  }

  put32(&buffer_, kLocalHeaderSignature);
  put16(&buffer_, kVersionNeeded);
  put16(&buffer_, 0);                  // flags
  put16(&buffer_, entry.method);
  put16(&buffer_, 0);                  // modification time
  put16(&buffer_, 0x0021);             // modification date (1980-01-01)
  put32(&buffer_, entry.crc);
  put32(&buffer_, entry.compressed_size);
  put32(&buffer_, entry.uncompressed_size);
  put16(&buffer_, static_cast<uint16_t>(path.size()));
  put16(&buffer_, extra_length);
  buffer_.insert(buffer_.end(), path.begin(), path.end());
  buffer_.insert(buffer_.end(), extra_length, 0);
  buffer_.insert(buffer_.end(), payload.begin(), payload.end());

  entries_.push_back(std::move(entry));
  return true;
}

bool ZipWriter::addText(const std::string& path, const std::string& text,
                        ZipCompression compression) {
  return add(path, std::vector<uint8_t>(text.begin(), text.end()), compression);
}

std::vector<uint8_t> ZipWriter::finish() {
  const uint32_t central_offset = static_cast<uint32_t>(buffer_.size());
  for (const Entry& entry : entries_) {
    put32(&buffer_, kCentralHeaderSignature);
    put16(&buffer_, kVersionNeeded);     // version made by
    put16(&buffer_, kVersionNeeded);     // version needed
    put16(&buffer_, 0);                  // flags
    put16(&buffer_, entry.method);
    put16(&buffer_, 0);                  // time
    put16(&buffer_, 0x0021);             // date
    put32(&buffer_, entry.crc);
    put32(&buffer_, entry.compressed_size);
    put32(&buffer_, entry.uncompressed_size);
    put16(&buffer_, static_cast<uint16_t>(entry.path.size()));
    put16(&buffer_, 0);                  // extra length
    put16(&buffer_, 0);                  // comment length
    put16(&buffer_, 0);                  // disk number
    put16(&buffer_, 0);                  // internal attributes
    put32(&buffer_, 0);                  // external attributes
    put32(&buffer_, entry.local_header_offset);
    buffer_.insert(buffer_.end(), entry.path.begin(), entry.path.end());
  }
  const uint32_t central_size = static_cast<uint32_t>(buffer_.size()) - central_offset;

  put32(&buffer_, kEndOfCentralDirSignature);
  put16(&buffer_, 0);  // this disk
  put16(&buffer_, 0);  // disk with central directory
  put16(&buffer_, static_cast<uint16_t>(entries_.size()));
  put16(&buffer_, static_cast<uint16_t>(entries_.size()));
  put32(&buffer_, central_size);
  put32(&buffer_, central_offset);
  put16(&buffer_, 0);  // comment length

  std::vector<uint8_t> result = std::move(buffer_);
  buffer_.clear();
  entries_.clear();
  return result;
}

namespace {

/// Locate the end-of-central-directory record by scanning backwards.
bool findEndOfCentralDirectory(const std::vector<uint8_t>& archive, size_t* out_offset) {
  if (archive.size() < 22) return false;
  const size_t limit = std::min<size_t>(archive.size(), 65557);  // 22 + max comment
  for (size_t back = 22; back <= limit; ++back) {
    const size_t offset = archive.size() - back;
    if (get32(archive.data() + offset) == kEndOfCentralDirSignature) {
      *out_offset = offset;
      return true;
    }
  }
  return false;
}

/// Walk the central directory, invoking `visit(path, method, csize, usize, local_offset)`.
template <typename Visitor>
bool walkCentralDirectory(const std::vector<uint8_t>& archive, const Visitor& visit) {
  size_t eocd = 0;
  if (!findEndOfCentralDirectory(archive, &eocd)) return false;
  const uint16_t count = get16(archive.data() + eocd + 10);
  const uint32_t central_offset = get32(archive.data() + eocd + 16);
  if (central_offset > archive.size()) return false;

  size_t pos = central_offset;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 46 > archive.size()) return false;
    if (get32(archive.data() + pos) != kCentralHeaderSignature) return false;
    const uint16_t method = get16(archive.data() + pos + 10);
    const uint32_t compressed = get32(archive.data() + pos + 20);
    const uint32_t uncompressed = get32(archive.data() + pos + 24);
    const uint16_t name_length = get16(archive.data() + pos + 28);
    const uint16_t extra_length = get16(archive.data() + pos + 30);
    const uint16_t comment_length = get16(archive.data() + pos + 32);
    const uint32_t local_offset = get32(archive.data() + pos + 42);
    if (pos + 46 + name_length > archive.size()) return false;
    const std::string path(reinterpret_cast<const char*>(archive.data() + pos + 46),
                           name_length);
    if (!visit(path, method, compressed, uncompressed, local_offset)) return false;
    pos += 46 + name_length + extra_length + comment_length;
  }
  return true;
}

/// Offset of an entry's file data, given its local header offset.
bool localDataOffset(const std::vector<uint8_t>& archive, uint32_t local_offset,
                     size_t* out) {
  if (local_offset + 30 > archive.size()) return false;
  if (get32(archive.data() + local_offset) != kLocalHeaderSignature) return false;
  const uint16_t name_length = get16(archive.data() + local_offset + 26);
  const uint16_t extra_length = get16(archive.data() + local_offset + 28);
  const size_t data = static_cast<size_t>(local_offset) + 30 + name_length + extra_length;
  if (data > archive.size()) return false;
  *out = data;
  return true;
}

}  // namespace

bool zipList(const std::vector<uint8_t>& archive, std::vector<std::string>* out) {
  out->clear();
  return walkCentralDirectory(
      archive, [out](const std::string& path, uint16_t, uint32_t, uint32_t, uint32_t) {
        out->push_back(path);
        return true;
      });
}

bool zipRead(const std::vector<uint8_t>& archive, std::vector<ZipEntry>* out) {
  out->clear();
  return walkCentralDirectory(
      archive, [&archive, out](const std::string& path, uint16_t method, uint32_t compressed,
                               uint32_t uncompressed, uint32_t local_offset) {
        if (!pathIsSafe(path)) return false;
        size_t data_offset = 0;
        if (!localDataOffset(archive, local_offset, &data_offset)) return false;
        if (data_offset + compressed > archive.size()) return false;

        ZipEntry entry;
        entry.path = path;
        if (method == kMethodStore) {
          if (compressed != uncompressed) return false;
          entry.data.assign(archive.begin() + data_offset,
                            archive.begin() + data_offset + compressed);
        } else if (method == kMethodDeflate) {
          if (!rawInflate(archive.data() + data_offset, compressed, uncompressed,
                          &entry.data)) {
            return false;
          }
        } else {
          return false;  // unsupported method
        }
        out->push_back(std::move(entry));
        return true;
      });
}

int64_t zipEntryDataOffset(const std::vector<uint8_t>& archive, const std::string& path) {
  int64_t result = -1;
  walkCentralDirectory(archive, [&archive, &path, &result](const std::string& name, uint16_t,
                                                           uint32_t, uint32_t,
                                                           uint32_t local_offset) {
    if (name != path) return true;
    size_t data_offset = 0;
    if (localDataOffset(archive, local_offset, &data_offset)) {
      result = static_cast<int64_t>(data_offset);
    }
    return true;
  });
  return result;
}

}  // namespace parallax
