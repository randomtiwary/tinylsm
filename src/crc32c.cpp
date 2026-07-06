#include "tinylsm/crc32c.h"

#include <array>

namespace tinylsm {
namespace crc32c {
namespace {

// Reflected Castagnoli polynomial used for table-driven CRC-32C.
// Unreflected poly is 0x1EDC6F41; bit-reflected form is 0x82F63B78.
constexpr uint32_t kPoly = 0x82F63B78u;

std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j) {
      if (c & 1u) {
        c = kPoly ^ (c >> 1);
      } else {
        c >>= 1;
      }
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256>& Table() {
  // Function-local static: initialized once on first use (C++11 thread-safe).
  static const std::array<uint32_t, 256> kTable = MakeTable();
  return kTable;
}

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  const auto& table = Table();
  // Standard CRC init/final XOR with 0xffffffff.
  crc = crc ^ 0xffffffffu;
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

}  // namespace crc32c
}  // namespace tinylsm
