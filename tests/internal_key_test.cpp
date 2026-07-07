#include "internal_key.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using tinylsm::EncodeInternalKey;
using tinylsm::ExtractSequence;
using tinylsm::ExtractUserKey;
using tinylsm::ExtractValueType;
using tinylsm::InternalKeyComparator;
using tinylsm::kMaxSequenceNumber;
using tinylsm::kTypeDeletion;
using tinylsm::kTypeValue;
using tinylsm::LookupKey;
using tinylsm::PackSequenceAndType;
using tinylsm::ParseInternalKey;
using tinylsm::ParsedInternalKey;
using tinylsm::SequenceNumber;

TEST(InternalKeyTest, EncodeDecodeRoundTrip) {
  const std::string user = "hello";
  const SequenceNumber seq = 0x123456789abcull;
  const auto encoded = EncodeInternalKey(user, seq, kTypeValue);
  ASSERT_EQ(encoded.size(), user.size() + 8);

  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(encoded, &parsed));
  EXPECT_EQ(parsed.user_key, user);
  EXPECT_EQ(parsed.sequence, seq);
  EXPECT_EQ(parsed.type, kTypeValue);

  EXPECT_EQ(ExtractUserKey(encoded), user);
  EXPECT_EQ(ExtractSequence(encoded), seq);
  EXPECT_EQ(ExtractValueType(encoded), kTypeValue);
}

TEST(InternalKeyTest, DeletionTypeRoundTrip) {
  const auto encoded = EncodeInternalKey("k", 42, kTypeDeletion);
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(encoded, &parsed));
  EXPECT_EQ(parsed.user_key, "k");
  EXPECT_EQ(parsed.sequence, 42u);
  EXPECT_EQ(parsed.type, kTypeDeletion);
  EXPECT_EQ(ExtractValueType(encoded), kTypeDeletion);
}

TEST(InternalKeyTest, TrailerIsLittleEndian) {
  // sequence = 1, type = kTypeValue (1) → packed = (1 << 8) | 1 = 0x101
  // LE bytes of 0x101: 01 01 00 00 00 00 00 00
  const auto encoded = EncodeInternalKey("ab", 1, kTypeValue);
  ASSERT_EQ(encoded.size(), 2u + 8u);
  EXPECT_EQ(encoded[0], 'a');
  EXPECT_EQ(encoded[1], 'b');
  EXPECT_EQ(static_cast<uint8_t>(encoded[2]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(encoded[3]), 0x01);
  for (int i = 4; i < 10; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(encoded[i]), 0x00) << "byte index " << i;
  }
}

TEST(InternalKeyTest, TrailerKnownVectorSeqAndType) {
  // seq = 0x01020304050607, type = kTypeDeletion (0)
  // packed = 0x0102030405060700
  // LE bytes: 00 07 06 05 04 03 02 01
  const SequenceNumber seq = 0x01020304050607ull;
  const auto encoded = EncodeInternalKey("", seq, kTypeDeletion);
  ASSERT_EQ(encoded.size(), 8u);
  const uint8_t expected[] = {0x00, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(encoded[i]), expected[i]) << "i=" << i;
  }
  EXPECT_EQ(PackSequenceAndType(seq, kTypeDeletion), 0x0102030405060700ull);
  EXPECT_EQ(ExtractSequence(encoded), seq);
  EXPECT_EQ(ExtractValueType(encoded), kTypeDeletion);
}

TEST(InternalKeyTest, EmptyUserKey) {
  const auto encoded = EncodeInternalKey("", 7, kTypeValue);
  ASSERT_EQ(encoded.size(), 8u);
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(encoded, &parsed));
  EXPECT_TRUE(parsed.user_key.empty());
  EXPECT_EQ(parsed.sequence, 7u);
  EXPECT_EQ(parsed.type, kTypeValue);
}

TEST(InternalKeyTest, ParseRejectsShortKey) {
  ParsedInternalKey parsed;
  EXPECT_FALSE(ParseInternalKey("", &parsed));
  EXPECT_FALSE(ParseInternalKey("short", &parsed));
  EXPECT_FALSE(ParseInternalKey(std::string(7, 'x'), &parsed));
}

TEST(InternalKeyTest, MaxSequenceFitsIn56Bits) {
  const auto encoded =
      EncodeInternalKey("uk", kMaxSequenceNumber, kTypeValue);
  EXPECT_EQ(ExtractSequence(encoded), kMaxSequenceNumber);
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(encoded, &parsed));
  EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
}

TEST(InternalKeyTest, ComparatorUserKeyAscending) {
  InternalKeyComparator cmp;
  const auto a = EncodeInternalKey("a", 1, kTypeValue);
  const auto b = EncodeInternalKey("b", 1, kTypeValue);
  EXPECT_LT(cmp.Compare(a, b), 0);
  EXPECT_GT(cmp.Compare(b, a), 0);
  EXPECT_EQ(cmp.Compare(a, a), 0);
}

TEST(InternalKeyTest, ComparatorSameUserKeyHigherSequenceFirst) {
  InternalKeyComparator cmp;
  // Same user key: sequence 10 must sort before sequence 5 (descending seq).
  const auto newer = EncodeInternalKey("key", 10, kTypeValue);
  const auto older = EncodeInternalKey("key", 5, kTypeValue);
  EXPECT_LT(cmp.Compare(newer, older), 0);
  EXPECT_GT(cmp.Compare(older, newer), 0);
  EXPECT_TRUE(cmp(newer, older));
  EXPECT_FALSE(cmp(older, newer));
}

TEST(InternalKeyTest, ComparatorValueBeforeDeletionOnEqualSequence) {
  // Same user key and sequence: type descending → value (1) before deletion (0).
  InternalKeyComparator cmp;
  const auto value = EncodeInternalKey("k", 3, kTypeValue);
  const auto del = EncodeInternalKey("k", 3, kTypeDeletion);
  EXPECT_LT(cmp.Compare(value, del), 0);
  EXPECT_GT(cmp.Compare(del, value), 0);
}

TEST(InternalKeyTest, ComparatorSortsMixedKeys) {
  InternalKeyComparator cmp;
  std::vector<std::string> keys = {
      EncodeInternalKey("b", 1, kTypeValue),
      EncodeInternalKey("a", 2, kTypeDeletion),
      EncodeInternalKey("a", 5, kTypeValue),
      EncodeInternalKey("a", 2, kTypeValue),
      EncodeInternalKey("c", 9, kTypeValue),
  };
  std::sort(keys.begin(), keys.end(), cmp);

  // Expected order: a@5 value, a@2 value, a@2 deletion, b@1, c@9
  EXPECT_EQ(ExtractUserKey(keys[0]), "a");
  EXPECT_EQ(ExtractSequence(keys[0]), 5u);
  EXPECT_EQ(ExtractValueType(keys[0]), kTypeValue);

  EXPECT_EQ(ExtractUserKey(keys[1]), "a");
  EXPECT_EQ(ExtractSequence(keys[1]), 2u);
  EXPECT_EQ(ExtractValueType(keys[1]), kTypeValue);

  EXPECT_EQ(ExtractUserKey(keys[2]), "a");
  EXPECT_EQ(ExtractSequence(keys[2]), 2u);
  EXPECT_EQ(ExtractValueType(keys[2]), kTypeDeletion);

  EXPECT_EQ(ExtractUserKey(keys[3]), "b");
  EXPECT_EQ(ExtractUserKey(keys[4]), "c");
}

TEST(InternalKeyTest, LookupKeyEncoding) {
  LookupKey lkey("foo", 99);
  EXPECT_EQ(lkey.user_key(), "foo");
  const auto expected = EncodeInternalKey("foo", 99, kTypeValue);
  EXPECT_EQ(lkey.internal_key(), expected);
  EXPECT_EQ(ExtractSequence(lkey.internal_key()), 99u);
  EXPECT_EQ(ExtractValueType(lkey.internal_key()), kTypeValue);
}

TEST(InternalKeyTest, LookupKeySeekOrdersBeforeOlderVersions) {
  // For Get at snapshot 100: seek key is (user, seq=100, type=value).
  // Real entries with seq<=100 should sort after the seek key when seq is lower,
  // and equal user key with seq 100 value should match the seek form.
  InternalKeyComparator cmp;
  LookupKey seek("k", 100);
  const auto at_snap_value = EncodeInternalKey("k", 100, kTypeValue);
  const auto older = EncodeInternalKey("k", 50, kTypeValue);
  const auto newer_invisible = EncodeInternalKey("k", 200, kTypeValue);

  EXPECT_EQ(cmp.Compare(seek.internal_key(), at_snap_value), 0);
  // Higher seq first: 200 sorts before seek@100; 50 sorts after.
  EXPECT_LT(cmp.Compare(newer_invisible, seek.internal_key()), 0);
  EXPECT_GT(cmp.Compare(older, seek.internal_key()), 0);
}

}  // namespace
