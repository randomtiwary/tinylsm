#pragma once

// SST file-level layout: footer + magic (docs/format.md §5.4 / §5.5).

#include "block_format.h"
#include "tinylsm/coding.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace tinylsm {

// Fixed footer length at EOF (format.md §5.4).
constexpr size_t kFooterSize = 48;

// Default target size for data block_contents before flush (format.md §5.5).
constexpr size_t kDefaultBlockSize = 4096;

// ASCII magic "TINYLSM1" in file order (do not byte-swap).
// Bytes: 54 49 4E 59 4C 53 4D 31
inline constexpr char kTableMagic[] = "TINYLSM1";
static_assert(sizeof(kTableMagic) == 9, "magic is 8 chars + NUL");

// Footer layout (exactly 48 bytes, little-endian fields):
//   [0..8)   index_handle.offset
//   [8..16)  index_handle.size     (contents only)
//   [16..24) filter_handle.offset  (0 if no filter)
//   [24..32) filter_handle.size
//   [32..40) padding_or_version    (must be 0 for TINYLSM1)
//   [40..48) magic "TINYLSM1"
struct Footer {
  BlockHandle index_handle;
  BlockHandle filter_handle;  // zero when bloom disabled (v1 default)
  // padding is always written as 0

  void EncodeTo(std::string* dst) const {
    PutFixed64(dst, index_handle.offset);
    PutFixed64(dst, index_handle.size);
    PutFixed64(dst, filter_handle.offset);
    PutFixed64(dst, filter_handle.size);
    PutFixed64(dst, 0);  // padding_or_version
    dst->append(kTableMagic, 8);
  }

  std::string Encode() const {
    std::string out;
    out.reserve(kFooterSize);
    EncodeTo(&out);
    return out;
  }

  Status DecodeFrom(std::string_view input) {
    if (input.size() < kFooterSize) {
      return Status::Corruption("footer too short");
    }
    if (std::memcmp(input.data() + 40, kTableMagic, 8) != 0) {
      return Status::Corruption("bad table magic");
    }
    const uint64_t pad = DecodeFixed64(input.data() + 32);
    if (pad != 0) {
      return Status::Corruption("nonzero footer padding");
    }
    index_handle.offset = DecodeFixed64(input.data() + 0);
    index_handle.size = DecodeFixed64(input.data() + 8);
    filter_handle.offset = DecodeFixed64(input.data() + 16);
    filter_handle.size = DecodeFixed64(input.data() + 24);
    return Status::OK();
  }
};

// Metadata returned by TableBuilder::Finish (FileMetaData-ish subset).
struct TableBuildStats {
  uint64_t file_size = 0;
  std::string smallest_key;  // first internal key added (empty if no keys)
  std::string largest_key;   // last internal key added
  uint64_t num_entries = 0;
  uint64_t num_data_blocks = 0;
};

}  // namespace tinylsm
