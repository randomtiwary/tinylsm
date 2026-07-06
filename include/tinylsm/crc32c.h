#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {
namespace crc32c {

// Castagnoli CRC-32C (poly 0x1EDC6F41), software table-based implementation.
// Matches LevelDB/RocksDB intent for hardware-friendly CRC over protected bytes.

// Return the CRC-32C of data[0,n-1].
uint32_t Value(const char* data, size_t n);

// Extend an existing CRC-32C with more data (crc is unmasked raw CRC).
// Value(data, n) == Extend(0, data, n).
uint32_t Extend(uint32_t crc, const char* data, size_t n);

// LevelDB-compatible mask applied before storing CRC as fixed32_le on disk.
// Prevents embedded CRCs from looking like valid adjacent CRCs of zeros, etc.
//
//   masked = ((crc >> 15) | (crc << 17)) + 0xa282ead8u
//
inline uint32_t Mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}

// Inverse of Mask (mandatory on every read path before comparing):
//
//   unmasked = masked - 0xa282ead8u
//   crc      = (unmasked << 15) | (unmasked >> 17)
//
inline uint32_t Unmask(uint32_t masked) {
  const uint32_t rot = masked - 0xa282ead8u;
  return (rot << 15) | (rot >> 17);
}

}  // namespace crc32c
}  // namespace tinylsm
