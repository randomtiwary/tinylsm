#include "block.h"
#include "block_builder.h"
#include "block_format.h"
#include "internal_key.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinylsm::AppendBlockTrailer;
using tinylsm::Block;
using tinylsm::BlockBuilder;
using tinylsm::BlockHandle;
using tinylsm::EncodeInternalKey;
using tinylsm::InternalKeyComparator;
using tinylsm::kBlockRestartInterval;
using tinylsm::kBlockTrailerSize;
using tinylsm::kNoCompression;
using tinylsm::kTypeValue;
using tinylsm::Status;
using tinylsm::VerifyBlockTrailer;
using tinylsm::crc32c::Mask;
using tinylsm::crc32c::Value;
using tinylsm::DecodeFixed32;
using tinylsm::EncodeFixed32;
using tinylsm::PutFixed32;

InternalKeyComparator cmp;

std::string IKey(std::string_view user, uint64_t seq = 1) {
  return EncodeInternalKey(user, seq, kTypeValue);
}

// Build contents (no trailer) from (key, value) pairs in order.
std::string BuildContents(
    const std::vector<std::pair<std::string, std::string>>& kvs,
    int restart_interval = kBlockRestartInterval) {
  BlockBuilder b(restart_interval);
  for (const auto& kv : kvs) {
    b.Add(kv.first, kv.second);
  }
  return std::string(b.Finish());
}

std::string WithTrailer(std::string contents) {
  AppendBlockTrailer(&contents);
  return contents;
}

// ---------------------------------------------------------------------------
// BlockHandle
// ---------------------------------------------------------------------------

TEST(BlockHandleTest, EncodeDecodeRoundTrip) {
  BlockHandle h;
  h.offset = 12345;
  h.size = 678;  // contents only
  std::string enc = h.Encode();

  std::string_view in(enc);
  BlockHandle out;
  ASSERT_TRUE(out.DecodeFrom(&in).ok());
  EXPECT_TRUE(in.empty());
  EXPECT_EQ(out.offset, h.offset);
  EXPECT_EQ(out.size, h.size);
}

TEST(BlockHandleTest, EncodeKnownSmallValues) {
  // offset=1, size=2 → varint bytes 0x01 0x02
  BlockHandle h{1, 2};
  const std::string enc = h.Encode();
  ASSERT_EQ(enc.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(enc[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(enc[1]), 0x02);
}

TEST(BlockHandleTest, DecodeTruncatedIsCorruption) {
  std::string_view in("\x80");  // incomplete varint
  BlockHandle h;
  Status s = h.DecodeFrom(&in);
  EXPECT_TRUE(s.IsCorruption());
}

// ---------------------------------------------------------------------------
// BlockBuilder: empty, multi-entry, restarts
// ---------------------------------------------------------------------------

TEST(BlockBuilderTest, EmptyBlock) {
  BlockBuilder b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.NumEntries(), 0);
  const std::string contents(b.Finish());
  // restart[0]=0 + num_restarts=1 → 8 bytes
  ASSERT_EQ(contents.size(), 8u);
  EXPECT_EQ(DecodeFixed32(contents.data()), 0u);
  EXPECT_EQ(DecodeFixed32(contents.data() + 4), 1u);

  Block block(contents);
  ASSERT_TRUE(block.ok()) << block.status().ToString();
  EXPECT_EQ(block.NumRestarts(), 1u);

  auto it = block.NewIterator(&cmp);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
  it.SeekToLast();
  EXPECT_FALSE(it.Valid());
  it.Seek(IKey("any"));
  EXPECT_FALSE(it.Valid());
}

TEST(BlockBuilderTest, MultiEntryRoundTrip) {
  const auto k1 = IKey("a");
  const auto k2 = IKey("b");
  const auto k3 = IKey("c");
  const std::string contents =
      BuildContents({{k1, "va"}, {k2, "vb"}, {k3, "vc"}});

  Block block(contents);
  ASSERT_TRUE(block.ok());
  auto it = block.NewIterator(&cmp);

  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), k1);
  EXPECT_EQ(it.value(), "va");

  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), k2);
  EXPECT_EQ(it.value(), "vb");

  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), k3);
  EXPECT_EQ(it.value(), "vc");

  it.Next();
  EXPECT_FALSE(it.Valid());

  it.SeekToLast();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), k3);
  EXPECT_EQ(it.value(), "vc");
}

TEST(BlockBuilderTest, RestartEveryInterval) {
  // Force restart interval of 2 for easier checking.
  constexpr int kInterval = 2;
  BlockBuilder b(kInterval);
  std::vector<std::string> keys;
  for (int i = 0; i < 5; ++i) {
    keys.push_back(IKey(std::string(1, static_cast<char>('a' + i))));
    b.Add(keys.back(), "v" + std::to_string(i));
  }
  const std::string contents(b.Finish());

  // Entries 0,2,4 start restarts → 3 restarts.
  Block block(contents);
  ASSERT_TRUE(block.ok());
  EXPECT_EQ(block.NumRestarts(), 3u);

  // Manually verify restart offsets are increasing and point at entry starts.
  // Layout ends with: r0 r1 r2 num=3
  const size_t n = contents.size();
  ASSERT_EQ(DecodeFixed32(contents.data() + n - 4), 3u);
  const uint32_t r0 = DecodeFixed32(contents.data() + n - 4 - 12);
  const uint32_t r1 = DecodeFixed32(contents.data() + n - 4 - 8);
  const uint32_t r2 = DecodeFixed32(contents.data() + n - 4 - 4);
  EXPECT_EQ(r0, 0u);
  EXPECT_GT(r1, r0);
  EXPECT_GT(r2, r1);
}

TEST(BlockBuilderTest, DefaultRestartIntervalIs16) {
  BlockBuilder b;
  for (int i = 0; i < 16; ++i) {
    // Distinct keys in order: "a00", "a01", ...
    char buf[8];
    std::snprintf(buf, sizeof(buf), "k%02d", i);
    b.Add(IKey(buf), "v");
  }
  // 16 entries, all in first restart region → still 1 restart.
  {
    BlockBuilder b1;
    for (int i = 0; i < 16; ++i) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "k%02d", i);
      b1.Add(IKey(buf), "v");
    }
    Block block(std::string(b1.Finish()));
    ASSERT_TRUE(block.ok());
    EXPECT_EQ(block.NumRestarts(), 1u);
  }
  // 17th entry opens a second restart.
  {
    BlockBuilder b2;
    for (int i = 0; i < 17; ++i) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "k%02d", i);
      b2.Add(IKey(buf), "v");
    }
    Block block(std::string(b2.Finish()));
    ASSERT_TRUE(block.ok());
    EXPECT_EQ(block.NumRestarts(), 2u);
  }
}

// ---------------------------------------------------------------------------
// Seek + Prev across restart boundaries
// ---------------------------------------------------------------------------

TEST(BlockIteratorTest, SeekExactAndMiss) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 40; ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "k%02d", i);
    kvs.emplace_back(IKey(buf), std::string("v") + buf);
  }
  Block block(BuildContents(kvs, /*restart_interval=*/16));
  ASSERT_TRUE(block.ok());
  EXPECT_EQ(block.NumRestarts(), 3u);  // 0..15, 16..31, 32..39
  auto it = block.NewIterator(&cmp);

  // Exact hit at restart boundary (index 16).
  it.Seek(IKey("k16"));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k16"));
  EXPECT_EQ(it.value(), "vk16");

  // Exact hit mid-interval.
  it.Seek(IKey("k05"));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k05"));

  // Seek past end.
  it.Seek(IKey("k99"));
  EXPECT_FALSE(it.Valid());

  // Seek before first → first key.
  it.Seek(IKey("a"));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k00"));

  // Seek to non-existent between keys → next key (k11).
  // Our keys are k00..k39; there is no gap in user keys if we only use those.
  // Use a user key between k10 and k11 lexicographically: "k10\xff" sorts after
  // "k10" trailer... actually internal keys compare user key first.
  // User key "k10a" is > "k10" and < "k11".
  it.Seek(IKey("k10a"));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k11"));
}

TEST(BlockIteratorTest, PrevAcrossRestartBoundary) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 20; ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "k%02d", i);
    kvs.emplace_back(IKey(buf), "v");
  }
  Block block(BuildContents(kvs, 16));
  auto it = block.NewIterator(&cmp);

  it.Seek(IKey("k16"));  // first key of second restart
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k16"));

  it.Prev();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), IKey("k15"));

  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  it.Prev();
  EXPECT_FALSE(it.Valid());
}

TEST(BlockIteratorTest, IndexBlockStyleHandles) {
  // Index block: key = last internal key of data block; value = BlockHandle.
  BlockHandle h0{0, 100};
  BlockHandle h1{105, 200};  // 100 contents + 5 trailer → next at 105
  const auto last0 = IKey("apple");
  const auto last1 = IKey("zebra");

  BlockBuilder b;
  b.Add(last0, h0.Encode());
  b.Add(last1, h1.Encode());
  Block block(std::string(b.Finish()));
  ASSERT_TRUE(block.ok());

  auto it = block.NewIterator(&cmp);
  it.Seek(IKey("banana"));  // between apple and zebra
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), last1);

  std::string_view val = it.value();
  BlockHandle decoded;
  ASSERT_TRUE(decoded.DecodeFrom(&val).ok());
  EXPECT_EQ(decoded, h1);
  EXPECT_EQ(decoded.size, 200u);  // contents only
}

// ---------------------------------------------------------------------------
// Trailer + CRC
// ---------------------------------------------------------------------------

TEST(BlockTrailerTest, AppendAndVerify) {
  const std::string contents = BuildContents({{IKey("k"), "v"}});
  std::string full = contents;
  AppendBlockTrailer(&full);
  ASSERT_EQ(full.size(), contents.size() + kBlockTrailerSize);
  EXPECT_EQ(static_cast<uint8_t>(full[contents.size()]), kNoCompression);

  std::string_view out;
  ASSERT_TRUE(VerifyBlockTrailer(full, &out).ok());
  EXPECT_EQ(out, contents);
}

TEST(BlockTrailerTest, CrcMismatchIsCorruption) {
  const std::string contents = BuildContents({{IKey("k"), "v"}});
  std::string full = WithTrailer(contents);
  // Flip a byte in the contents region.
  full[0] = static_cast<char>(full[0] ^ 0xff);

  std::string_view out;
  Status s = VerifyBlockTrailer(full, &out);
  EXPECT_TRUE(s.IsCorruption());

  Block block = Block::FromTrailerBuffer(full);
  EXPECT_FALSE(block.ok());
  EXPECT_TRUE(block.status().IsCorruption());
}

TEST(BlockTrailerTest, CrcCoversTypeByte) {
  const std::string contents = BuildContents({{IKey("k"), "v"}});
  std::string full = contents;
  AppendBlockTrailer(&full);
  // Corrupt only the compression type byte (still "valid" 0→1).
  full[contents.size()] = 1;
  // Even if we only checked type, type!=0 fails; also recompute expected:
  // verify rejects unsupported compression.
  std::string_view out;
  Status s = VerifyBlockTrailer(full, &out);
  EXPECT_TRUE(s.IsCorruption());
}

TEST(BlockTrailerTest, FromTrailerBufferRoundTrip) {
  const auto k = IKey("hello");
  const std::string contents = BuildContents({{k, "world"}});
  const std::string full = WithTrailer(contents);

  Block block = Block::FromTrailerBuffer(full);
  ASSERT_TRUE(block.ok()) << block.status().ToString();
  auto it = block.NewIterator(&cmp);
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), k);
  EXPECT_EQ(it.value(), "world");
}

TEST(BlockTrailerTest, MaskedCrcLayout) {
  // Empty block contents (8 bytes) + trailer.
  BlockBuilder b;
  std::string contents(b.Finish());
  std::string full = contents;
  AppendBlockTrailer(&full);

  const char type = full[contents.size()];
  EXPECT_EQ(static_cast<uint8_t>(type), 0);

  uint32_t crc = Value(contents.data(), contents.size());
  crc = tinylsm::crc32c::Extend(crc, &type, 1);
  const uint32_t expected_masked = Mask(crc);
  const uint32_t stored =
      DecodeFixed32(full.data() + contents.size() + 1);
  EXPECT_EQ(stored, expected_masked);
}

TEST(BlockBuilderTest, CurrentSizeEstimateMatchesFinish) {
  BlockBuilder b;
  b.Add(IKey("aa"), "1");
  b.Add(IKey("bb"), "22");
  const size_t est = b.CurrentSizeEstimate();
  const std::string contents(b.Finish());
  EXPECT_EQ(est, contents.size());
}

}  // namespace
