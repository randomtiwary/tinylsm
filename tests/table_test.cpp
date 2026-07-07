#include "internal_key.h"
#include "table.h"
#include "table_builder.h"
#include "table_format.h"
#include "tinylsm/coding.h"
#include "tinylsm/env.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinylsm::BlockHandle;
using tinylsm::DecodeFixed64;
using tinylsm::EncodeInternalKey;
using tinylsm::Env;
using tinylsm::Footer;
using tinylsm::kFooterSize;
using tinylsm::kTableMagic;
using tinylsm::kTypeDeletion;
using tinylsm::kTypeValue;
using tinylsm::LookupKey;
using tinylsm::RandomAccessFile;
using tinylsm::Status;
using tinylsm::Table;
using tinylsm::TableBuilder;
using tinylsm::TableBuildStats;
using tinylsm::WritableFile;

class TableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    ASSERT_TRUE(env_->NewTempDir(&tmpdir_).ok());
  }

  void TearDown() override {
    if (tmpdir_.empty()) {
      return;
    }
    std::vector<std::string> children;
    if (env_->GetChildren(tmpdir_, &children).ok()) {
      for (const auto& name : children) {
        (void)env_->DeleteFile(tmpdir_ + "/" + name);
      }
    }
    (void)env_->DeleteDir(tmpdir_);
  }

  std::string Path(const std::string& name) const {
    return tmpdir_ + "/" + name;
  }

  static std::string IKey(std::string_view user, uint64_t seq = 1,
                          tinylsm::ValueType type = kTypeValue) {
    return EncodeInternalKey(user, seq, type);
  }

  // Build an SST at path; returns build stats.
  Status BuildTable(
      const std::string& path,
      const std::vector<std::pair<std::string, std::string>>& kvs,
      TableBuildStats* stats, size_t block_size = 4096) {
    std::unique_ptr<WritableFile> w;
    Status s = env_->NewWritableFile(path, &w);
    if (!s.ok()) {
      return s;
    }
    TableBuilder builder(w.get(), block_size);
    for (const auto& kv : kvs) {
      builder.Add(kv.first, kv.second);
    }
    s = builder.Finish(stats);
    if (!s.ok()) {
      return s;
    }
    return w->Close();
  }

  Status OpenTable(const std::string& path, std::unique_ptr<Table>* table,
                   std::unique_ptr<RandomAccessFile>* file_out) {
    uint64_t size = 0;
    Status s = env_->GetFileSize(path, &size);
    if (!s.ok()) {
      return s;
    }
    s = env_->NewRandomAccessFile(path, file_out);
    if (!s.ok()) {
      return s;
    }
    return Table::Open(file_out->get(), size, table);
  }

  Env* env_ = nullptr;
  std::string tmpdir_;
};

// ---------------------------------------------------------------------------
// Footer unit tests
// ---------------------------------------------------------------------------

TEST(FooterTest, EncodeDecodeRoundTrip) {
  Footer f;
  f.index_handle = BlockHandle{100, 200};
  f.filter_handle = BlockHandle{0, 0};
  const std::string enc = f.Encode();
  ASSERT_EQ(enc.size(), kFooterSize);
  EXPECT_EQ(std::string_view(enc.data() + 40, 8), kTableMagic);

  Footer out;
  ASSERT_TRUE(out.DecodeFrom(enc).ok());
  EXPECT_EQ(out.index_handle, f.index_handle);
  EXPECT_EQ(out.filter_handle, f.filter_handle);
}

TEST(FooterTest, BadMagicIsCorruption) {
  Footer f;
  f.index_handle = BlockHandle{1, 2};
  std::string enc = f.Encode();
  enc[40] = 'X';
  Footer out;
  Status s = out.DecodeFrom(enc);
  EXPECT_TRUE(s.IsCorruption());
}

TEST(FooterTest, ShortBufferIsCorruption) {
  Footer out;
  EXPECT_TRUE(out.DecodeFrom("short").IsCorruption());
}

// ---------------------------------------------------------------------------
// Empty table
// ---------------------------------------------------------------------------

TEST_F(TableTest, EmptyTable) {
  const std::string path = Path("empty.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {}, &stats).ok());
  EXPECT_EQ(stats.num_entries, 0u);
  EXPECT_EQ(stats.num_data_blocks, 1u);  // one empty data block
  EXPECT_TRUE(stats.smallest_key.empty());
  EXPECT_TRUE(stats.largest_key.empty());
  EXPECT_GE(stats.file_size, kFooterSize);

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok()) << "open empty";
  ASSERT_NE(table, nullptr);

  // Filter handle zero.
  EXPECT_EQ(table->footer().filter_handle.offset, 0u);
  EXPECT_EQ(table->footer().filter_handle.size, 0u);

  std::string value;
  Status s = table->Get("missing", /*snapshot=*/100, &value);
  EXPECT_TRUE(s.IsNotFound());

  s = table->Get(IKey("a"), &value);
  EXPECT_TRUE(s.IsNotFound());
}

// ---------------------------------------------------------------------------
// Single-block / multi-key
// ---------------------------------------------------------------------------

TEST_F(TableTest, SingleBlockMultipleKeys) {
  const std::string path = Path("single.sst");
  const auto k1 = IKey("apple", 10);
  const auto k2 = IKey("banana", 10);
  const auto k3 = IKey("cherry", 10);

  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{k1, "v1"}, {k2, "v2"}, {k3, "v3"}}, &stats)
                  .ok());
  EXPECT_EQ(stats.num_entries, 3u);
  EXPECT_EQ(stats.smallest_key, k1);
  EXPECT_EQ(stats.largest_key, k3);
  EXPECT_EQ(stats.num_data_blocks, 1u);

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());

  std::string value;
  ASSERT_TRUE(table->Get("apple", 10, &value).ok());
  EXPECT_EQ(value, "v1");
  ASSERT_TRUE(table->Get("banana", 10, &value).ok());
  EXPECT_EQ(value, "v2");
  ASSERT_TRUE(table->Get("cherry", 10, &value).ok());
  EXPECT_EQ(value, "v3");

  // Misses.
  EXPECT_TRUE(table->Get("avocado", 10, &value).IsNotFound());
  EXPECT_TRUE(table->Get("date", 10, &value).IsNotFound());
  EXPECT_TRUE(table->Get("a", 10, &value).IsNotFound());
}

// ---------------------------------------------------------------------------
// Multi-block table (tiny block_size forces many blocks)
// ---------------------------------------------------------------------------

TEST_F(TableTest, MultiBlockTable) {
  const std::string path = Path("multi.sst");
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 50; ++i) {
    char ukey[16];
    std::snprintf(ukey, sizeof(ukey), "k%03d", i);
    kvs.emplace_back(IKey(ukey, 5), std::string("val-") + ukey);
  }

  // Tiny target forces a new block after nearly every entry.
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, kvs, &stats, /*block_size=*/64).ok());
  EXPECT_EQ(stats.num_entries, 50u);
  EXPECT_GT(stats.num_data_blocks, 1u);

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());

  std::string value;
  // Hits across blocks.
  ASSERT_TRUE(table->Get("k000", 5, &value).ok());
  EXPECT_EQ(value, "val-k000");
  ASSERT_TRUE(table->Get("k025", 5, &value).ok());
  EXPECT_EQ(value, "val-k025");
  ASSERT_TRUE(table->Get("k049", 5, &value).ok());
  EXPECT_EQ(value, "val-k049");

  // Misses.
  EXPECT_TRUE(table->Get("k050", 5, &value).IsNotFound());
  EXPECT_TRUE(table->Get("j999", 5, &value).IsNotFound());
  EXPECT_TRUE(table->Get("k00", 5, &value).IsNotFound());
}

// ---------------------------------------------------------------------------
// Sequence / deletion semantics via LookupKey
// ---------------------------------------------------------------------------

TEST_F(TableTest, SequenceAndDeletion) {
  const std::string path = Path("seq.sst");
  // Same user key, newer first in sort order (seq descending).
  // Table order for user "foo": seq=20 value, seq=10 deletion, seq=5 value
  // But we must Add in comparator order: highest seq first for same user key.
  const auto k20 = IKey("foo", 20, kTypeValue);
  const auto k10 = IKey("foo", 10, kTypeDeletion);
  const auto k05 = IKey("foo", 5, kTypeValue);
  const auto other = IKey("bar", 1, kTypeValue);

  TableBuildStats stats;
  // Order: bar@1, then foo@20, foo@10, foo@5 (user_key asc, then seq desc).
  ASSERT_TRUE(BuildTable(path,
                         {{other, "bar-v"},
                          {k20, "new"},
                          {k10, ""},
                          {k05, "old"}},
                         &stats)
                  .ok());

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());

  std::string value;
  // Snapshot sees seq<=20 → "new"
  ASSERT_TRUE(table->Get("foo", 20, &value).ok());
  EXPECT_EQ(value, "new");
  // Snapshot 15: hides seq 20, lands on deletion @10 → NotFound
  EXPECT_TRUE(table->Get("foo", 15, &value).IsNotFound());
  // Snapshot 10: deletion
  EXPECT_TRUE(table->Get("foo", 10, &value).IsNotFound());
  // Snapshot 7: value at seq 5
  ASSERT_TRUE(table->Get("foo", 7, &value).ok());
  EXPECT_EQ(value, "old");

  ASSERT_TRUE(table->Get("bar", 100, &value).ok());
  EXPECT_EQ(value, "bar-v");
}

// ---------------------------------------------------------------------------
// Corruption: bad magic / bad CRC
// ---------------------------------------------------------------------------

TEST_F(TableTest, BadMagicOnOpen) {
  const std::string path = Path("badmagic.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{IKey("k"), "v"}}, &stats).ok());

  // Read whole file, corrupt magic, rewrite.
  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(path, &size).ok());
  std::unique_ptr<RandomAccessFile> raf;
  ASSERT_TRUE(env_->NewRandomAccessFile(path, &raf).ok());
  std::string bytes;
  ASSERT_TRUE(raf->Read(0, static_cast<size_t>(size), &bytes).ok());
  ASSERT_EQ(bytes.size(), size);
  raf.reset();

  ASSERT_GE(bytes.size(), kFooterSize);
  bytes[bytes.size() - 8] = 'X';  // first byte of magic

  {
    std::unique_ptr<WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append(bytes).ok());
    ASSERT_TRUE(w->Close().ok());
  }

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  Status s = OpenTable(path, &table, &file);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_EQ(table, nullptr);
}

TEST_F(TableTest, BadCrcOnGet) {
  const std::string path = Path("badcrc.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{IKey("k"), "v"}}, &stats).ok());

  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(path, &size).ok());
  std::unique_ptr<RandomAccessFile> raf;
  ASSERT_TRUE(env_->NewRandomAccessFile(path, &raf).ok());
  std::string bytes;
  ASSERT_TRUE(raf->Read(0, static_cast<size_t>(size), &bytes).ok());
  raf.reset();

  // Flip a byte in the first data block contents (offset 0).
  bytes[0] = static_cast<char>(bytes[0] ^ 0xff);

  {
    std::unique_ptr<WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append(bytes).ok());
    ASSERT_TRUE(w->Close().ok());
  }

  // Open may still succeed (index CRC ok); Get loads data block and fails.
  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());
  std::string value;
  Status s = table->Get("k", 1, &value);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(TableTest, CorruptIndexCrcOnOpen) {
  const std::string path = Path("badindex.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{IKey("k"), "v"}}, &stats).ok());

  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(path, &size).ok());
  std::unique_ptr<RandomAccessFile> raf;
  ASSERT_TRUE(env_->NewRandomAccessFile(path, &raf).ok());
  std::string bytes;
  ASSERT_TRUE(raf->Read(0, static_cast<size_t>(size), &bytes).ok());
  raf.reset();

  // Footer index_handle.offset at end-48; flip a byte of the index block.
  ASSERT_GE(bytes.size(), kFooterSize);
  const uint64_t index_off =
      DecodeFixed64(bytes.data() + bytes.size() - kFooterSize);
  ASSERT_LT(index_off, bytes.size() - kFooterSize);
  bytes[static_cast<size_t>(index_off)] =
      static_cast<char>(bytes[static_cast<size_t>(index_off)] ^ 0xff);

  {
    std::unique_ptr<WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append(bytes).ok());
    ASSERT_TRUE(w->Close().ok());
  }

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  Status s = OpenTable(path, &table, &file);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// ---------------------------------------------------------------------------
// Get via full internal key (LookupKey object)
// ---------------------------------------------------------------------------

TEST_F(TableTest, GetWithLookupKeyObject) {
  const std::string path = Path("lookup.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{IKey("alpha", 3), "A"}, {IKey("beta", 3), "B"}},
                         &stats)
                  .ok());

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());

  LookupKey lk("beta", 3);
  std::string value;
  ASSERT_TRUE(table->Get(lk.internal_key(), &value).ok());
  EXPECT_EQ(value, "B");

  LookupKey miss("gamma", 3);
  EXPECT_TRUE(table->Get(miss.internal_key(), &value).IsNotFound());
}

TEST_F(TableTest, TooShortFileIsCorruption) {
  const std::string path = Path("tiny.sst");
  {
    std::unique_ptr<WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append("not a real sst").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  Status s = OpenTable(path, &table, &file);
  EXPECT_TRUE(s.IsCorruption());
}

TEST_F(TableTest, AbandonDoesNotRequireFinish) {
  const std::string path = Path("abandon.sst");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
  TableBuilder builder(w.get());
  builder.Add(IKey("x"), "y");
  builder.Abandon();
  // Finish after abandon is invalid (assert in debug); just close the file.
  ASSERT_TRUE(w->Close().ok());
}

}  // namespace

// ---------------------------------------------------------------------------
// Optional bloom filter (filter_handle non-zero)
// ---------------------------------------------------------------------------

TEST_F(TableTest, BloomEnabledNoFalseNegativesAndMisses) {
  const std::string path = Path("bloom.sst");
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 50; ++i) {
    char ukey[16];
    std::snprintf(ukey, sizeof(ukey), "bk%03d", i);
    kvs.emplace_back(IKey(ukey, 1), std::string("v-") + ukey);
  }

  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
  // bits_per_key=10 enables filter block.
  TableBuilder builder(w.get(), /*block_size=*/64, /*bloom_bits_per_key=*/10);
  for (const auto& kv : kvs) {
    builder.Add(kv.first, kv.second);
  }
  TableBuildStats stats;
  ASSERT_TRUE(builder.Finish(&stats).ok());
  ASSERT_TRUE(w->Close().ok());

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());

  // Filter present and non-empty.
  EXPECT_NE(table->footer().filter_handle.offset, 0u);
  EXPECT_NE(table->footer().filter_handle.size, 0u);
  EXPECT_TRUE(table->has_filter());
  EXPECT_FALSE(table->filter().empty());

  std::string value;
  for (int i = 0; i < 50; ++i) {
    char ukey[16];
    std::snprintf(ukey, sizeof(ukey), "bk%03d", i);
    ASSERT_TRUE(table->Get(ukey, 1, &value).ok()) << ukey;
    EXPECT_EQ(value, std::string("v-") + ukey);
  }

  // Absent keys must not return a value (bloom may short-circuit or fall through).
  EXPECT_TRUE(table->Get("zz-missing", 1, &value).IsNotFound());
  EXPECT_TRUE(table->Get("aa-missing", 1, &value).IsNotFound());
  EXPECT_TRUE(table->Get("bk999", 1, &value).IsNotFound());
}

TEST_F(TableTest, BloomDisabledKeepsZeroFilterHandle) {
  const std::string path = Path("nobloom.sst");
  TableBuildStats stats;
  ASSERT_TRUE(BuildTable(path, {{IKey("k"), "v"}}, &stats).ok());

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());
  EXPECT_EQ(table->footer().filter_handle.offset, 0u);
  EXPECT_EQ(table->footer().filter_handle.size, 0u);
  EXPECT_FALSE(table->has_filter());
  EXPECT_TRUE(table->filter().empty());

  std::string value;
  ASSERT_TRUE(table->Get("k", 1, &value).ok());
  EXPECT_EQ(value, "v");
}

TEST_F(TableTest, BloomEmptyTableHasNoFilterBlock) {
  // No user keys → filter_handle stays zero even if bloom requested.
  const std::string path = Path("empty-bloom.sst");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
  TableBuilder builder(w.get(), 4096, /*bloom_bits_per_key=*/10);
  TableBuildStats stats;
  ASSERT_TRUE(builder.Finish(&stats).ok());
  ASSERT_TRUE(w->Close().ok());

  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path, &table, &file).ok());
  EXPECT_EQ(table->footer().filter_handle.offset, 0u);
  EXPECT_EQ(table->footer().filter_handle.size, 0u);
}
