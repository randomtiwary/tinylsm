#pragma once

// SST block primitives: BlockHandle, trailer, restart interval (docs/format.md §5).
// Internal to the library (same visibility as internal_key.h).

#include "tinylsm/coding.h"
#include "tinylsm/crc32c.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace tinylsm {

// Restart every N entries in a data/index block (format.md §5.2 / §10).
constexpr int kBlockRestartInterval = 16;

// Trailer: 1-byte compression type + 4-byte masked CRC32C (format.md §5.1).
constexpr size_t kBlockTrailerSize = 5;

// v1 always stores uncompressed block contents.
constexpr uint8_t kNoCompression = 0;

// Points at block_contents only — size excludes the 5-byte trailer.
struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // contents length only

  void EncodeTo(std::string* dst) const {
    PutVarint64(dst, offset);
    PutVarint64(dst, size);
  }

  std::string Encode() const {
    std::string out;
    EncodeTo(&out);
    return out;
  }

  // Consume varint64 offset + varint64 size from the front of *input.
  Status DecodeFrom(std::string_view* input) {
    uint64_t off = 0;
    uint64_t sz = 0;
    if (!GetVarint64(input, &off) || !GetVarint64(input, &sz)) {
      return Status::Corruption("bad block handle");
    }
    offset = off;
    size = sz;
    return Status::OK();
  }

  bool operator==(const BlockHandle& o) const {
    return offset == o.offset && size == o.size;
  }
  bool operator!=(const BlockHandle& o) const { return !(*this == o); }
};

// Append the 5-byte trailer for the block contents currently in *buf
// (or equivalently: CRC over (contents || type), then mask).
// On entry, *buf must be exactly the block_contents bytes.
// On return, *buf is contents || type || masked_crc (fixed32 LE).
inline void AppendBlockTrailer(std::string* buf) {
  const char type = static_cast<char>(kNoCompression);
  // CRC protects contents + the compression type byte (format.md §5.1).
  uint32_t crc = crc32c::Value(buf->data(), buf->size());
  crc = crc32c::Extend(crc, &type, 1);
  buf->push_back(type);
  PutFixed32(buf, crc32c::Mask(crc));
}

// Given a buffer of contents || trailer, verify type and CRC.
// On success, *contents views the prefix of full_block excluding the trailer.
// full_block must outlive *contents.
inline Status VerifyBlockTrailer(std::string_view full_block,
                                 std::string_view* contents) {
  if (full_block.size() < kBlockTrailerSize) {
    return Status::Corruption("block too short for trailer");
  }
  const size_t contents_size = full_block.size() - kBlockTrailerSize;
  const char type = full_block[contents_size];
  if (static_cast<uint8_t>(type) != kNoCompression) {
    return Status::Corruption("unsupported block compression type");
  }
  const uint32_t stored_masked =
      DecodeFixed32(full_block.data() + contents_size + 1);
  const uint32_t stored = crc32c::Unmask(stored_masked);

  uint32_t crc = crc32c::Value(full_block.data(), contents_size);
  crc = crc32c::Extend(crc, &type, 1);
  if (crc != stored) {
    return Status::Corruption("block CRC mismatch");
  }
  *contents = std::string_view(full_block.data(), contents_size);
  return Status::OK();
}

}  // namespace tinylsm
