#pragma once

// Internal keys: user_key || fixed64_le((sequence << 8) | value_type).
// Used by memtable and SSTables. Not part of the installed public API.

#include "tinylsm/coding.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace tinylsm {

// Sequence numbers use the high 56 bits of the packed trailer (max 2^56 - 1).
using SequenceNumber = uint64_t;
constexpr SequenceNumber kMaxSequenceNumber =
    (static_cast<uint64_t>(1) << 56) - 1;

// Low 8 bits of the packed trailer.
enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
};

// Pack sequence (high 56) and type (low 8) into a logical uint64 before LE encode.
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  return (seq << 8) | static_cast<uint8_t>(type);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

// Encode user_key || fixed64_le((seq << 8) | type).
std::string EncodeInternalKey(std::string_view user_key, SequenceNumber seq,
                              ValueType type);

// Append the same encoding to *dst.
void AppendInternalKey(std::string* dst, std::string_view user_key,
                       SequenceNumber seq, ValueType type);

// Parse an encoded internal key. Returns false if shorter than 8 bytes.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

// Extractors. For keys shorter than 8 bytes, ExtractUserKey returns empty;
// sequence/type helpers return 0 / kTypeDeletion (callers should reject short keys).
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  if (internal_key.size() < 8) {
    return std::string_view();
  }
  return std::string_view(internal_key.data(), internal_key.size() - 8);
}

inline SequenceNumber ExtractSequence(std::string_view internal_key) {
  if (internal_key.size() < 8) {
    return 0;
  }
  const uint64_t packed =
      DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  return packed >> 8;
}

inline ValueType ExtractValueType(std::string_view internal_key) {
  if (internal_key.size() < 8) {
    return kTypeDeletion;
  }
  const uint64_t packed =
      DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  return static_cast<ValueType>(packed & 0xff);
}

// Total order for internal keys:
//   1. user_key ascending (bytewise)
//   2. sequence descending (higher sequence first)
//   3. type descending (higher type first; kTypeValue before kTypeDeletion)
class InternalKeyComparator {
 public:
  // Returns < 0 if a < b, 0 if equal, > 0 if a > b.
  int Compare(std::string_view a, std::string_view b) const;

  // Strict weak ordering for use with std algorithms / ordered containers.
  bool operator()(std::string_view a, std::string_view b) const {
    return Compare(a, b) < 0;
  }

  const char* Name() const { return "tinylsm.InternalKeyComparator"; }
};

// Seek key for Get: user_key || fixed64_le((snapshot_seq << 8) | kTypeValue).
// With sequence-descending order, seeking this finds the newest entry with
// seq <= snapshot_seq for that user key when scanning forward.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);

  // Full internal-key encoding (user_key + trailer).
  std::string_view internal_key() const { return internal_key_; }

  // User key portion only.
  std::string_view user_key() const {
    return std::string_view(internal_key_.data(),
                            internal_key_.size() - 8);
  }

 private:
  std::string space_;
  std::string_view internal_key_;
};

}  // namespace tinylsm
