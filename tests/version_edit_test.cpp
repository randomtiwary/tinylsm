#include "version_edit.h"

#include "internal_key.h"
#include "tinylsm/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

// Hex encode for golden / debug assertions (lowercase).
std::string ToHex(std::string_view bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xf]);
  }
  return out;
}

std::string FromHex(std::string_view hex) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  EXPECT_EQ(hex.size() % 2, 0u);
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    EXPECT_GE(hi, 0);
    EXPECT_GE(lo, 0);
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

TEST(VersionEditTest, EmptyEncodeDecode) {
  tinylsm::VersionEdit edit;
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_TRUE(encoded.empty());

  tinylsm::VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_FALSE(decoded.HasComparatorName());
  EXPECT_FALSE(decoded.HasLogNumber());
  EXPECT_TRUE(decoded.NewFiles().empty());
  EXPECT_TRUE(decoded.DeletedFiles().empty());
}

TEST(VersionEditTest, SeedEditRoundTrip) {
  tinylsm::VersionEdit edit;
  edit.SetComparatorName(tinylsm::kComparatorName);
  edit.SetLogNumber(1);
  edit.SetNextFileNumber(2);
  edit.SetLastSequence(0);

  std::string encoded;
  edit.EncodeTo(&encoded);
  ASSERT_FALSE(encoded.empty());

  tinylsm::VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok()) << "hex=" << ToHex(encoded);
  EXPECT_TRUE(decoded.HasComparatorName());
  EXPECT_EQ(decoded.ComparatorName(), tinylsm::kComparatorName);
  EXPECT_TRUE(decoded.HasLogNumber());
  EXPECT_EQ(decoded.LogNumber(), 1u);
  EXPECT_TRUE(decoded.HasNextFileNumber());
  EXPECT_EQ(decoded.NextFileNumber(), 2u);
  EXPECT_TRUE(decoded.HasLastSequence());
  EXPECT_EQ(decoded.LastSequence(), 0u);
}

TEST(VersionEditTest, AddAndDeleteFileRoundTrip) {
  const std::string smallest =
      tinylsm::EncodeInternalKey("aaa", 10, tinylsm::kTypeValue);
  const std::string largest =
      tinylsm::EncodeInternalKey("zzz", 20, tinylsm::kTypeDeletion);

  tinylsm::VersionEdit edit;
  edit.AddFile(/*level=*/0, /*file=*/7, /*file_size=*/4096, smallest, largest);
  edit.DeleteFile(/*level=*/1, /*file=*/3);
  edit.SetLastSequence(99);

  std::string encoded;
  edit.EncodeTo(&encoded);

  // Byte identity via hex intermediate (round-trip through hex string).
  const std::string hex = ToHex(encoded);
  const std::string from_hex = FromHex(hex);
  EXPECT_EQ(from_hex, encoded);

  tinylsm::VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(from_hex).ok());
  ASSERT_EQ(decoded.NewFiles().size(), 1u);
  EXPECT_EQ(decoded.NewFiles()[0].first, 0);
  EXPECT_EQ(decoded.NewFiles()[0].second->number, 7u);
  EXPECT_EQ(decoded.NewFiles()[0].second->file_size, 4096u);
  EXPECT_EQ(decoded.NewFiles()[0].second->smallest, smallest);
  EXPECT_EQ(decoded.NewFiles()[0].second->largest, largest);

  ASSERT_EQ(decoded.DeletedFiles().size(), 1u);
  EXPECT_EQ(*decoded.DeletedFiles().begin(), std::make_pair(1, uint64_t{3}));
  EXPECT_EQ(decoded.LastSequence(), 99u);
}

TEST(VersionEditTest, KnownTagLayoutHex) {
  // Minimal log-number-only edit: tag=2 (kLogNumber), varint64=1 → bytes 02 01
  tinylsm::VersionEdit edit;
  edit.SetLogNumber(1);
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_EQ(ToHex(encoded), "0201");

  // Comparator with empty name would be odd; use short name "x":
  // tag=1, len=1, 'x' → 01 01 78
  tinylsm::VersionEdit c;
  c.SetComparatorName("x");
  encoded.clear();
  c.EncodeTo(&encoded);
  EXPECT_EQ(ToHex(encoded), "010178");
}

TEST(VersionEditTest, SkipsUnusedTags) {
  // Build a payload with compact pointer (tag 5) and prev log (tag 9) then
  // a real log number field, and ensure decode ignores unused tags.
  std::string payload;
  // tag 5 compact pointer: level=0, key="k"
  tinylsm::PutVarint32(&payload, 5);
  tinylsm::PutVarint32(&payload, 0);
  tinylsm::PutLengthPrefixedSlice(&payload, "k");
  // tag 9 prev log number = 42
  tinylsm::PutVarint32(&payload, 9);
  tinylsm::PutVarint64(&payload, 42);
  // tag 2 log number = 7
  tinylsm::PutVarint32(&payload, 2);
  tinylsm::PutVarint64(&payload, 7);

  tinylsm::VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(payload).ok());
  EXPECT_TRUE(decoded.HasLogNumber());
  EXPECT_EQ(decoded.LogNumber(), 7u);
}

TEST(VersionEditTest, CorruptionUnknownTag) {
  std::string payload;
  tinylsm::PutVarint32(&payload, 99);  // unknown
  tinylsm::VersionEdit decoded;
  auto s = decoded.DecodeFrom(payload);
  EXPECT_TRUE(s.IsCorruption());
}

TEST(VersionEditTest, DoubleEncodeStable) {
  tinylsm::VersionEdit edit;
  edit.SetComparatorName(tinylsm::kComparatorName);
  edit.SetLogNumber(5);
  edit.SetNextFileNumber(10);
  edit.SetLastSequence(100);
  const std::string ik =
      tinylsm::EncodeInternalKey("key", 1, tinylsm::kTypeValue);
  edit.AddFile(0, 3, 100, ik, ik);

  std::string a, b;
  edit.EncodeTo(&a);
  edit.EncodeTo(&b);
  EXPECT_EQ(a, b);

  tinylsm::VersionEdit again;
  ASSERT_TRUE(again.DecodeFrom(a).ok());
  std::string c;
  again.EncodeTo(&c);
  // Re-encode after decode should be equivalent for set fields.
  tinylsm::VersionEdit third;
  ASSERT_TRUE(third.DecodeFrom(c).ok());
  EXPECT_EQ(third.LogNumber(), 5u);
  EXPECT_EQ(third.NewFiles().size(), 1u);
}

}  // namespace
