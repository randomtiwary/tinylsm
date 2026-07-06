#include "tinylsm/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST(CodingTest, Fixed32RoundTrip) {
  const uint32_t values[] = {
      0u, 1u, 0x7fu, 0x80u, 0xffu, 0x100u, 0xffffu, 0x10000u,
      0xffffffu, 0x12345678u, 0xffffffffu,
  };
  for (uint32_t v : values) {
    char buf[4];
    tinylsm::EncodeFixed32(buf, v);
    EXPECT_EQ(tinylsm::DecodeFixed32(buf), v);

    std::string s;
    tinylsm::PutFixed32(&s, v);
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(tinylsm::DecodeFixed32(s.data()), v);
  }
}

TEST(CodingTest, Fixed32IsLittleEndian) {
  char buf[4];
  tinylsm::EncodeFixed32(buf, 0x04030201u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x03);
  EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0x04);
}

TEST(CodingTest, Fixed64RoundTrip) {
  const uint64_t values[] = {
      0ull,
      1ull,
      0xffull,
      0x123456789abcdef0ull,
      0xffffffffffffffffull,
      (1ull << 32),
      (1ull << 40) | 0x99ull,
  };
  for (uint64_t v : values) {
    char buf[8];
    tinylsm::EncodeFixed64(buf, v);
    EXPECT_EQ(tinylsm::DecodeFixed64(buf), v);

    std::string s;
    tinylsm::PutFixed64(&s, v);
    ASSERT_EQ(s.size(), 8u);
    EXPECT_EQ(tinylsm::DecodeFixed64(s.data()), v);
  }
}

TEST(CodingTest, Fixed64IsLittleEndian) {
  char buf[8];
  tinylsm::EncodeFixed64(buf, 0x0807060504030201ull);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(buf[i]), static_cast<uint8_t>(i + 1));
  }
}

TEST(CodingTest, Varint32RoundTrip) {
  const uint32_t values[] = {
      0u, 1u, 127u, 128u, 16383u, 16384u, 0xffffu, 0x10000u,
      0xffffffu, 0xffffffffu, 300u,
  };
  for (uint32_t v : values) {
    char buf[tinylsm::kMaxVarint32Length];
    char* end = tinylsm::EncodeVarint32(buf, v);
    uint32_t decoded = 0;
    const char* p = tinylsm::GetVarint32Ptr(buf, end, &decoded);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p, end);
    EXPECT_EQ(decoded, v);

    std::string s;
    tinylsm::PutVarint32(&s, v);
    std::string_view in(s);
    uint32_t got = 0;
    ASSERT_TRUE(tinylsm::GetVarint32(&in, &got));
    EXPECT_EQ(got, v);
    EXPECT_TRUE(in.empty());
  }
}

TEST(CodingTest, Varint32KnownEncodings) {
  // Single-byte values < 128 encode as themselves.
  char buf[tinylsm::kMaxVarint32Length];
  char* end = tinylsm::EncodeVarint32(buf, 0);
  EXPECT_EQ(end - buf, 1);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0);

  end = tinylsm::EncodeVarint32(buf, 127);
  EXPECT_EQ(end - buf, 1);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 127);

  // 300 = 0b1_00101100 → bytes 0xAC 0x02 (LevelDB/LEB128).
  end = tinylsm::EncodeVarint32(buf, 300);
  ASSERT_EQ(end - buf, 2);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xAC);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x02);
}

TEST(CodingTest, Varint64RoundTrip) {
  const uint64_t values[] = {
      0ull,
      1ull,
      127ull,
      128ull,
      0xffffffffull,
      0x100000000ull,
      0xffffffffffffffffull,
      0x123456789abcdef0ull,
  };
  for (uint64_t v : values) {
    char buf[tinylsm::kMaxVarint64Length];
    char* end = tinylsm::EncodeVarint64(buf, v);
    uint64_t decoded = 0;
    const char* p = tinylsm::GetVarint64Ptr(buf, end, &decoded);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p, end);
    EXPECT_EQ(decoded, v);

    std::string s;
    tinylsm::PutVarint64(&s, v);
    std::string_view in(s);
    uint64_t got = 0;
    ASSERT_TRUE(tinylsm::GetVarint64(&in, &got));
    EXPECT_EQ(got, v);
    EXPECT_TRUE(in.empty());
  }
}

TEST(CodingTest, VarintTruncatedFails) {
  // Incomplete multi-byte varint: continuation bit set, no following byte.
  char bad[] = {static_cast<char>(0x80)};
  uint32_t v32 = 0;
  EXPECT_EQ(tinylsm::GetVarint32Ptr(bad, bad + 1, &v32), nullptr);
  uint64_t v64 = 0;
  EXPECT_EQ(tinylsm::GetVarint64Ptr(bad, bad + 1, &v64), nullptr);

  std::string_view in(bad, 1);
  EXPECT_FALSE(tinylsm::GetVarint32(&in, &v32));
}

TEST(CodingTest, LengthPrefixedRoundTrip) {
  const std::string payloads[] = {
      "",
      "a",
      "hello",
      std::string(200, 'x'),
      std::string("\0\1\2", 3),
  };
  for (const auto& payload : payloads) {
    std::string buf;
    tinylsm::PutLengthPrefixedSlice(&buf, payload);

    std::string_view in(buf);
    std::string_view got;
    ASSERT_TRUE(tinylsm::GetLengthPrefixedSlice(&in, &got));
    EXPECT_EQ(got, std::string_view(payload));
    EXPECT_TRUE(in.empty());
  }
}

TEST(CodingTest, LengthPrefixedMultiple) {
  std::string buf;
  tinylsm::PutLengthPrefixedSlice(&buf, "foo");
  tinylsm::PutLengthPrefixedSlice(&buf, "bar");
  tinylsm::PutFixed32(&buf, 42);

  std::string_view in(buf);
  std::string_view a, b;
  ASSERT_TRUE(tinylsm::GetLengthPrefixedSlice(&in, &a));
  ASSERT_TRUE(tinylsm::GetLengthPrefixedSlice(&in, &b));
  EXPECT_EQ(a, "foo");
  EXPECT_EQ(b, "bar");
  ASSERT_GE(in.size(), 4u);
  EXPECT_EQ(tinylsm::DecodeFixed32(in.data()), 42u);
}

TEST(CodingTest, LengthPrefixedTooShortFails) {
  std::string buf;
  tinylsm::PutVarint32(&buf, 5);  // claims 5 bytes
  buf.append("ab");               // only 2 present
  std::string_view in(buf);
  std::string_view got;
  EXPECT_FALSE(tinylsm::GetLengthPrefixedSlice(&in, &got));
}

}  // namespace
