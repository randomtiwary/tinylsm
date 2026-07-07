#include "internal_key.h"
#include "memtable.h"
#include "skiplist.h"

#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using tinylsm::EncodeInternalKey;
using tinylsm::ExtractSequence;
using tinylsm::ExtractUserKey;
using tinylsm::ExtractValueType;
using tinylsm::InternalKeyComparator;
using tinylsm::kTypeDeletion;
using tinylsm::kTypeValue;
using tinylsm::LookupKey;
using tinylsm::MemTable;
using tinylsm::SequenceNumber;
using tinylsm::SkipList;
using tinylsm::Status;

struct StringLess {
  int Compare(const std::string& a, const std::string& b) const {
    if (a < b) {
      return -1;
    }
    if (a > b) {
      return 1;
    }
    return 0;
  }
};

TEST(SkipListTest, Empty) {
  SkipList<std::string, StringLess> list(StringLess{});
  EXPECT_FALSE(list.Contains("a"));
  SkipList<std::string, StringLess>::Iterator it(&list);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek("a");
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, InsertContainsAndOrder) {
  SkipList<std::string, StringLess> list(StringLess{});
  list.Insert("b");
  list.Insert("a");
  list.Insert("c");
  EXPECT_TRUE(list.Contains("a"));
  EXPECT_TRUE(list.Contains("b"));
  EXPECT_TRUE(list.Contains("c"));
  EXPECT_FALSE(list.Contains("d"));

  SkipList<std::string, StringLess>::Iterator it(&list);
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "a");
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "b");
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "c");
  it.Next();
  EXPECT_FALSE(it.Valid());

  it.Seek("b");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "b");
  it.Seek("bb");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "c");
}

TEST(MemTableTest, EmptyGetNotFound) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  LookupKey lkey("missing", 100);
  std::string value;
  Status s;
  // Absent: false so layered Get may fall through to older layers.
  EXPECT_FALSE(table.Get(lkey, &value, &s));
  EXPECT_TRUE(s.IsNotFound());
}

TEST(MemTableTest, PutAndGet) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "foo", "bar");
  table.Add(2, kTypeValue, "baz", "qux");

  std::string value;
  Status s;
  ASSERT_TRUE(table.Get(LookupKey("foo", 100), &value, &s));
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(value, "bar");

  ASSERT_TRUE(table.Get(LookupKey("baz", 100), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "qux");
}

TEST(MemTableTest, DeleteIsNotFoundButStopsLayerSearch) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "k", "v");
  table.Add(2, kTypeDeletion, "k", "");

  std::string value = "stale";
  Status s;
  // Tombstone: true + NotFound — DBImpl must not fall through to SST.
  ASSERT_TRUE(table.Get(LookupKey("k", 100), &value, &s));
  EXPECT_TRUE(s.IsNotFound());
}

TEST(MemTableTest, OverwriteByHigherSequence) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "k", "old");
  table.Add(5, kTypeValue, "k", "new");

  std::string value;
  Status s;
  // Snapshot at seq 100 sees the newest entry (seq 5).
  ASSERT_TRUE(table.Get(LookupKey("k", 100), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "new");

  // Snapshot at seq 3 sees only seq 1 (seq 5 is newer than snapshot).
  ASSERT_TRUE(table.Get(LookupKey("k", 3), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "old");

  // Snapshot at seq 1 sees seq 1.
  ASSERT_TRUE(table.Get(LookupKey("k", 1), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "old");
}

TEST(MemTableTest, SnapshotDoesNotSeeFutureWrites) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(10, kTypeValue, "k", "future");

  std::string value;
  Status s;
  EXPECT_FALSE(table.Get(LookupKey("k", 5), &value, &s));
  EXPECT_TRUE(s.IsNotFound());
}

TEST(MemTableTest, DeleteThenPutVisibleAgain) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "k", "v1");
  table.Add(2, kTypeDeletion, "k", "");
  table.Add(3, kTypeValue, "k", "v2");

  std::string value;
  Status s;
  ASSERT_TRUE(table.Get(LookupKey("k", 100), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "v2");

  ASSERT_TRUE(table.Get(LookupKey("k", 2), &value, &s));
  EXPECT_TRUE(s.IsNotFound());

  ASSERT_TRUE(table.Get(LookupKey("k", 1), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "v1");
}

TEST(MemTableTest, IteratorOrder) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  // Insert out of order; iteration must be internal-key order:
  // user_key ascending, sequence descending within a user key.
  table.Add(1, kTypeValue, "b", "vb1");
  table.Add(3, kTypeValue, "a", "va3");
  table.Add(2, kTypeValue, "a", "va2");
  table.Add(1, kTypeValue, "c", "vc1");

  auto it = table.NewIterator();
  it.SeekToFirst();

  std::vector<std::pair<std::string, SequenceNumber>> seen;
  for (; it.Valid(); it.Next()) {
    seen.emplace_back(std::string(ExtractUserKey(it.key())),
                      ExtractSequence(it.key()));
  }

  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0].first, "a");
  EXPECT_EQ(seen[0].second, 3u);
  EXPECT_EQ(seen[1].first, "a");
  EXPECT_EQ(seen[1].second, 2u);
  EXPECT_EQ(seen[2].first, "b");
  EXPECT_EQ(seen[2].second, 1u);
  EXPECT_EQ(seen[3].first, "c");
  EXPECT_EQ(seen[3].second, 1u);
}

TEST(MemTableTest, IteratorValuesAndSeek) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "a", "1");
  table.Add(2, kTypeValue, "b", "2");
  table.Add(3, kTypeValue, "c", "3");

  auto it = table.NewIterator();
  it.Seek(EncodeInternalKey("b", 100, kTypeValue));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(ExtractUserKey(it.key()), "b");
  EXPECT_EQ(it.value(), "2");

  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(ExtractUserKey(it.key()), "c");
  EXPECT_EQ(it.value(), "3");
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(MemTableTest, MemoryUsageGrows) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  const size_t empty = table.ApproximateMemoryUsage();
  EXPECT_EQ(empty, 0u);

  table.Add(1, kTypeValue, "key", "value");
  const size_t after_one = table.ApproximateMemoryUsage();
  EXPECT_GT(after_one, empty);

  for (int i = 0; i < 50; ++i) {
    table.Add(static_cast<SequenceNumber>(i + 2), kTypeValue,
              "k" + std::to_string(i), std::string(64, 'x'));
  }
  EXPECT_GT(table.ApproximateMemoryUsage(), after_one);
}

TEST(MemTableTest, EmptyValue) {
  InternalKeyComparator cmp;
  MemTable table(cmp);
  table.Add(1, kTypeValue, "empty", "");
  std::string value = "not-empty";
  Status s;
  ASSERT_TRUE(table.Get(LookupKey("empty", 1), &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_TRUE(value.empty());
}

}  // namespace
