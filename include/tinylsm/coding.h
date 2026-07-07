#pragma once

// Little-endian fixed-width and LEB128 varint helpers for on-disk formats.
// Educational clarity preferred over micro-optimizations.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace tinylsm {

// Maximum encoded lengths for varints (LEB128, 7 bits per byte).
constexpr int kMaxVarint32Length = 5;
constexpr int kMaxVarint64Length = 10;

// ---------------------------------------------------------------------------
// Fixed-width little-endian integers
// ---------------------------------------------------------------------------

inline void EncodeFixed32(char* dst, uint32_t value) {
  auto* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  auto* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  buffer[4] = static_cast<uint8_t>(value >> 32);
  buffer[5] = static_cast<uint8_t>(value >> 40);
  buffer[6] = static_cast<uint8_t>(value >> 48);
  buffer[7] = static_cast<uint8_t>(value >> 56);
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const auto* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const auto* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint64_t>(buffer[0])) |
         (static_cast<uint64_t>(buffer[1]) << 8) |
         (static_cast<uint64_t>(buffer[2]) << 16) |
         (static_cast<uint64_t>(buffer[3]) << 24) |
         (static_cast<uint64_t>(buffer[4]) << 32) |
         (static_cast<uint64_t>(buffer[5]) << 40) |
         (static_cast<uint64_t>(buffer[6]) << 48) |
         (static_cast<uint64_t>(buffer[7]) << 56);
}

// Append fixed-width LE integers to a string.
inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Unsigned LEB128 varints (LevelDB-style)
// ---------------------------------------------------------------------------

// Encode v into dst; returns pointer past the last written byte.
// dst must have room for kMaxVarint32Length / kMaxVarint64Length bytes.
inline char* EncodeVarint32(char* dst, uint32_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) {
    *ptr++ = static_cast<uint8_t>(v | 0x80);
    v >>= 7;
  }
  *ptr++ = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

inline char* EncodeVarint64(char* dst, uint64_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) {
    *ptr++ = static_cast<uint8_t>(v | 0x80);
    v >>= 7;
  }
  *ptr++ = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  char buf[kMaxVarint32Length];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  char buf[kMaxVarint64Length];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

// Decode from [p, limit). On success, *value is set and the pointer past the
// varint is returned. On failure, returns nullptr.
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = static_cast<uint8_t>(*p);
    p++;
    if (byte < 0x80) {
      result |= (byte << shift);
      *value = result;
      return p;
    }
    result |= ((byte & 0x7f) << shift);
  }
  return nullptr;
}

inline const char* GetVarint64Ptr(const char* p, const char* limit,
                                  uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = static_cast<uint8_t>(*p);
    p++;
    if (byte < 0x80) {
      result |= (byte << shift);
      *value = result;
      return p;
    }
    result |= ((byte & 0x7f) << shift);
  }
  return nullptr;
}

// Consume a varint from the front of *input. Advances *input on success.
inline bool GetVarint32(std::string_view* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = std::string_view(q, static_cast<size_t>(limit - q));
  return true;
}

inline bool GetVarint64(std::string_view* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = std::string_view(q, static_cast<size_t>(limit - q));
  return true;
}

// ---------------------------------------------------------------------------
// Length-prefixed slices: varint32 length + raw bytes
// ---------------------------------------------------------------------------

inline void PutLengthPrefixedSlice(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

// On success, *result views the payload bytes inside *input's original buffer
// and *input is advanced past the payload.
inline bool GetLengthPrefixedSlice(std::string_view* input,
                                   std::string_view* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) {
    return false;
  }
  if (input->size() < len) {
    return false;
  }
  *result = std::string_view(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace tinylsm
