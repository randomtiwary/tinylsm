#include "tinylsm/crc32c.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

TEST(Crc32cTest, Empty) {
  // CRC of empty buffer is 0.
  EXPECT_EQ(tinylsm::crc32c::Value("", 0), 0u);
  EXPECT_EQ(tinylsm::crc32c::Value(nullptr, 0), 0u);
}

TEST(Crc32cTest, KnownVectors) {
  // Standard CRC-32C (Castagnoli) check value for "123456789".
  const char* check = "123456789";
  EXPECT_EQ(tinylsm::crc32c::Value(check, 9), 0xe3069283u);

  // Single byte "a".
  EXPECT_EQ(tinylsm::crc32c::Value("a", 1), 0xc1d04330u);

  // Common phrase vector.
  const char* fox = "The quick brown fox jumps over the lazy dog";
  EXPECT_EQ(tinylsm::crc32c::Value(fox, std::strlen(fox)), 0x22620404u);
}

TEST(Crc32cTest, ExtendMatchesValue) {
  const std::string data = "abcdefghijklmnop";
  const uint32_t whole = tinylsm::crc32c::Value(data.data(), data.size());

  uint32_t partial = tinylsm::crc32c::Value(data.data(), 5);
  partial = tinylsm::crc32c::Extend(partial, data.data() + 5, data.size() - 5);
  EXPECT_EQ(partial, whole);

  // Extend from 0 equals Value.
  EXPECT_EQ(tinylsm::crc32c::Extend(0, data.data(), data.size()), whole);
}

TEST(Crc32cTest, MaskUnmaskRoundTrip) {
  const uint32_t samples[] = {
      0u,
      1u,
      0xffffffffu,
      0xe3069283u,
      0xa282ead8u,
      0x12345678u,
      0x80000000u,
  };
  for (uint32_t crc : samples) {
    const uint32_t masked = tinylsm::crc32c::Mask(crc);
    EXPECT_EQ(tinylsm::crc32c::Unmask(masked), crc) << "crc=" << crc;
  }
}

TEST(Crc32cTest, MaskChangesValue) {
  // Mask should not be identity for typical CRCs (except accidental collisions).
  const uint32_t crc = 0xe3069283u;
  EXPECT_NE(tinylsm::crc32c::Mask(crc), crc);
}

TEST(Crc32cTest, MaskOfKnownVector) {
  // Document that mask of the "123456789" CRC is stable (regression guard).
  const uint32_t crc = tinylsm::crc32c::Value("123456789", 9);
  ASSERT_EQ(crc, 0xe3069283u);
  const uint32_t masked = tinylsm::crc32c::Mask(crc);
  // ((0xe3069283 >> 15) | (0xe3069283 << 17)) + 0xa282ead8
  // = (0x1c60d | 0x25060000) + 0xa282ead8 = 0x2507c60d + 0xa282ead8
  EXPECT_EQ(masked, 0xc78ab0e5u);
  EXPECT_EQ(tinylsm::crc32c::Unmask(masked), crc);
}

TEST(Crc32cTest, ValueThenMaskUnmask) {
  const char* payload = "wal-record-payload";
  const uint32_t crc = tinylsm::crc32c::Value(payload, std::strlen(payload));
  const uint32_t stored = tinylsm::crc32c::Mask(crc);
  const uint32_t recovered = tinylsm::crc32c::Unmask(stored);
  EXPECT_EQ(recovered, crc);
  EXPECT_EQ(tinylsm::crc32c::Value(payload, std::strlen(payload)), recovered);
}

}  // namespace
