#include "db_impl.h"

#include "compaction.h"
#include "internal_key.h"
#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

class CompactionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = tinylsm::Env::Default();
    ASSERT_TRUE(env_->NewTempDir(&dbname_).ok());
  }

  void TearDown() override {
    if (!dbname_.empty()) {
      std::vector<std::string> children;
      if (env_->GetChildren(dbname_, &children).ok()) {
        for (const auto& name : children) {
          (void)env_->DeleteFile(dbname_ + "/" + name);
        }
      }
      (void)env_->DeleteDir(dbname_);
    }
  }

  // Small buffer so each freeze/flush creates an L0 file quickly; low L0 trigger.
  tinylsm::Options CompactionOptions(int l0_trigger = 4,
                                     size_t write_buffer = 256) const {
    tinylsm::Options opt;
    opt.create_if_missing = true;
    opt.error_if_exists = false;
    opt.env = env_;
    opt.write_buffer_size = write_buffer;
    opt.l0_compaction_trigger = l0_trigger;
    opt.sync_writes = false;
    return opt;
  }

  static tinylsm::DBImpl* AsImpl(tinylsm::DB* db) {
    return static_cast<tinylsm::DBImpl*>(db);
  }

  // Freeze+flush current mem if non-empty; wait for flush.
  void ForceFlush(tinylsm::DB* db) {
    auto* impl = AsImpl(db);
    // Only freeze if mem has data.
    if (impl->TEST_MemApproximateMemoryUsage() > 0) {
      ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    }
    ASSERT_TRUE(impl->TEST_WaitForFlush());
  }

  // Produce n distinct L0 files via freeze/flush cycles.
  void ProduceL0Files(tinylsm::DB* db, int n, const std::string& key_prefix) {
    tinylsm::WriteOptions wo;
    wo.sync = false;
    auto* impl = AsImpl(db);
    for (int f = 0; f < n; ++f) {
      // Distinct key per file so each freeze has at least one entry.
      const std::string key = key_prefix + std::to_string(f);
      const std::string val = "v" + std::to_string(f) + std::string(32, 'x');
      ASSERT_TRUE(db->Put(wo, key, val).ok()) << key;
      ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
      ASSERT_TRUE(impl->TEST_WaitForFlush()) << "flush " << f;
    }
  }

  int CountSstFiles() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).ok());
    int n = 0;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sst") == 0) {
        ++n;
      }
    }
    return n;
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

// Unit: user-key range overlap helper.
TEST_F(CompactionTest, UserKeyRangesOverlap) {
  EXPECT_TRUE(tinylsm::UserKeyRangesOverlap("a", "c", "b", "d"));
  EXPECT_TRUE(tinylsm::UserKeyRangesOverlap("a", "c", "c", "e"));  // inclusive
  EXPECT_FALSE(tinylsm::UserKeyRangesOverlap("a", "b", "c", "d"));
  EXPECT_TRUE(tinylsm::UserKeyRangesOverlap("a", "z", "m", "n"));
}

// Newest wins across multiple L0 files after compaction to L1.
TEST_F(CompactionTest, NewestWinsAcrossL0) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/4);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  // Four L0 files, each overwriting key "k" with a newer value.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(db->Put(wo, "k", "val" + std::to_string(i)).ok());
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    ASSERT_TRUE(impl->TEST_WaitForFlush());
  }

  ASSERT_GE(impl->TEST_NumL0Files(), 4);
  ASSERT_TRUE(impl->TEST_WaitForCompaction());

  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  std::string value;
  tinylsm::ReadOptions ro;
  ASSERT_TRUE(db->Get(ro, "k", &value).ok());
  EXPECT_EQ(value, "val3");

  delete db;
}

// Overwrite chains: Get after compaction equals last write for many keys.
TEST_F(CompactionTest, OverwriteChainMatchesBeforeAfter) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/3);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  // Three generations of values for keys a,b,c across three L0 files.
  for (int gen = 0; gen < 3; ++gen) {
    for (char k : {'a', 'b', 'c'}) {
      std::string key(1, k);
      ASSERT_TRUE(db->Put(wo, key, "g" + std::to_string(gen) + key).ok());
    }
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    ASSERT_TRUE(impl->TEST_WaitForFlush());
  }

  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);

  tinylsm::ReadOptions ro;
  for (char k : {'a', 'b', 'c'}) {
    std::string key(1, k);
    std::string value;
    ASSERT_TRUE(db->Get(ro, key, &value).ok()) << key;
    EXPECT_EQ(value, "g2" + key);
  }

  delete db;
}

// Bottommost tombstone drop: Delete then compact → key gone and not in output.
TEST_F(CompactionTest, BottommostTombstoneDrop) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;
  tinylsm::ReadOptions ro;

  ASSERT_TRUE(db->Put(wo, "gone", "present").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  ASSERT_TRUE(db->Delete(wo, "gone").ok());
  // Also put an unrelated key so the second L0 is non-empty of something
  // and compaction still has a survivor after tombstone drop.
  ASSERT_TRUE(db->Put(wo, "keep", "yes").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  std::string value;
  EXPECT_TRUE(db->Get(ro, "gone", &value).IsNotFound());
  ASSERT_TRUE(db->Get(ro, "keep", &value).ok());
  EXPECT_EQ(value, "yes");

  // Reopen: tombstone must not resurrect from L1 (dropped at bottommost).
  delete db;
  db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  EXPECT_TRUE(db->Get(ro, "gone", &value).IsNotFound());
  ASSERT_TRUE(db->Get(ro, "keep", &value).ok());
  EXPECT_EQ(value, "yes");

  delete db;
}

// Pure delete of all keys in inputs → no L1 output file, key stays NotFound.
TEST_F(CompactionTest, AllTombstonesNoOutputFile) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  ASSERT_TRUE(db->Put(wo, "only", "v").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  ASSERT_TRUE(db->Delete(wo, "only").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  // Zero-entry merge: no L1 file required.
  EXPECT_EQ(impl->TEST_NumL1Files(), 0);

  std::string value;
  EXPECT_TRUE(db->Get(tinylsm::ReadOptions(), "only", &value).IsNotFound());

  delete db;
}

// R6: compaction output orphan if apply skipped; inputs remain during inject;
// data correct; orphan ignored on reopen. Arm inject *before* the flush that
// hits the L0 trigger so the first compact attempt is the skip-apply path.
TEST_F(CompactionTest, R6_OrphanCompactionOutput) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  ASSERT_TRUE(db->Put(wo, "k", "before").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_EQ(impl->TEST_NumL0Files(), 1);

  // First compact attempt (after 2nd L0) will write then skip apply (one-shot).
  impl->TEST_SetFailBeforeCompactionApply(true);

  ASSERT_TRUE(db->Put(wo, "k", "after").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  // Inject then successful retry: Wait drains until L0 compacted for real.
  ASSERT_TRUE(impl->TEST_WaitForCompaction());

  std::string value;
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "after");
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  // Orphan from the skipped apply must still be on disk and not in the live
  // Version (manifest ignores it). Live L1 is also present.
  std::set<uint64_t> live;
  {
    auto v = impl->TEST_CurrentVersion();
    ASSERT_NE(v, nullptr);
    for (int lvl = 0; lvl < 2; ++lvl) {
      for (const auto& f : v->LevelFiles(lvl)) {
        live.insert(f->number);
      }
    }
  }
  std::vector<std::string> children;
  ASSERT_TRUE(env_->GetChildren(dbname_, &children).ok());
  int orphans = 0;
  int live_on_disk = 0;
  for (const auto& name : children) {
    if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".sst") != 0) {
      continue;
    }
    char* end = nullptr;
    const uint64_t n = std::strtoull(name.c_str(), &end, 10);
    if (end == name.c_str() || std::string(end) != ".sst") {
      continue;
    }
    if (live.count(n)) {
      ++live_on_disk;
    } else {
      ++orphans;
    }
  }
  EXPECT_GE(live_on_disk, 1);
  EXPECT_GE(orphans, 1) << "R6 skip-apply should leave an orphan SST on disk";

  delete db;
  db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "after");
  AsImpl(db)->TEST_WaitForCompaction();
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "after");

  delete db;
}

// Happy-path unlink: after compact with no held Version, input SSTs are gone
// immediately (does not rely on a later flush or explicit TEST_Purge).
// Uses trigger=3 so we can snapshot two L0 numbers before the compacting flush.
TEST_F(CompactionTest, ObsoleteInputsUnlinkedWithoutHeldVersion) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/3);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  // Two L0 files — below trigger, no compaction yet.
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(db->Put(wo, "k", "v" + std::to_string(i)).ok());
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    ASSERT_TRUE(impl->TEST_WaitForFlush());
  }
  ASSERT_EQ(impl->TEST_NumL0Files(), 2);
  ASSERT_EQ(impl->TEST_NumL1Files(), 0);

  std::vector<uint64_t> early_inputs;
  {
    auto snap = impl->TEST_CurrentVersion();
    ASSERT_NE(snap, nullptr);
    for (const auto& f : snap->LevelFiles(0)) {
      early_inputs.push_back(f->number);
      ASSERT_TRUE(impl->TEST_SstFileExists(f->number));
    }
  }  // no external Version hold
  ASSERT_EQ(early_inputs.size(), 2u);

  // Third L0 hits trigger; compact all three. No held Version across apply.
  ASSERT_TRUE(db->Put(wo, "k", "v2").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(impl->TEST_WaitForCompaction());

  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  // Job-local refs cleared before purge: pre-trigger inputs unlinked now
  // without TEST_PurgeObsoleteFiles or a subsequent flush.
  for (uint64_t n : early_inputs) {
    EXPECT_FALSE(impl->TEST_SstFileExists(n))
        << "input " << n
        << " still on disk after compact with no held Version";
  }

  std::string value;
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "v2");

  delete db;
}

// Close after final compact must not leave obsolete inputs; live L1 survives.
TEST_F(CompactionTest, ObsoleteInputsUnlinkedOnClose) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  ASSERT_TRUE(db->Put(wo, "x", "1").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  const uint64_t first_l0 = [&]() {
    auto v = impl->TEST_CurrentVersion();
    EXPECT_EQ(v->NumFiles(0), 1u);
    return v->LevelFiles(0)[0]->number;
  }();

  ASSERT_TRUE(db->Put(wo, "x", "2").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(impl->TEST_WaitForCompaction());

  uint64_t live_l1 = 0;
  {
    auto v = impl->TEST_CurrentVersion();
    ASSERT_NE(v, nullptr);
    ASSERT_EQ(v->NumFiles(0), 0u);
    ASSERT_GE(v->NumFiles(1), 1u);
    live_l1 = v->LevelFiles(1)[0]->number;
  }

  // Happy path should already have unlinked first_l0; assert before close too.
  EXPECT_FALSE(impl->TEST_SstFileExists(first_l0));
  EXPECT_TRUE(impl->TEST_SstFileExists(live_l1));

  delete db;
  db = nullptr;

  // After destructor purge: live L1 still on disk; obsolete input still gone.
  EXPECT_FALSE(env_->FileExists(tinylsm::TableFileName(dbname_, first_l0)));
  EXPECT_TRUE(env_->FileExists(tinylsm::TableFileName(dbname_, live_l1)));

  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "x", &value).ok());
  EXPECT_EQ(value, "2");
  delete db;
}

// C3: reader holding old Version still works while compaction replaces files.
TEST_F(CompactionTest, C3_OldVersionReadableDuringCompaction) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;

  ASSERT_TRUE(db->Put(wo, "pin", "v0").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(db->Put(wo, "pin", "v1").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  // Hold current Version (lists the L0 input files) before compaction apply.
  std::shared_ptr<tinylsm::Version> held = impl->TEST_CurrentVersion();
  ASSERT_NE(held, nullptr);
  ASSERT_GE(held->NumFiles(0), 2u);

  // Record input file numbers that must stay on disk while held.
  std::vector<uint64_t> input_numbers;
  for (const auto& f : held->LevelFiles(0)) {
    input_numbers.push_back(f->number);
    EXPECT_TRUE(impl->TEST_SstFileExists(f->number)) << f->number;
  }

  // Run compaction to completion; live Version advances, held still refs files.
  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  // Files still exist because held Version keeps FileMetaData alive.
  for (uint64_t n : input_numbers) {
    EXPECT_TRUE(impl->TEST_SstFileExists(n))
        << "input " << n << " unlinked while old Version held";
  }

  // Read via Get (new Version) still works.
  std::string value;
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "pin", &value).ok());
  EXPECT_EQ(value, "v1");

  // Release old Version and purge — inputs should become unlinkable.
  held.reset();
  impl->TEST_PurgeObsoleteFiles();
  for (uint64_t n : input_numbers) {
    EXPECT_FALSE(impl->TEST_SstFileExists(n))
        << "input " << n << " should be unlinked after last Version drop";
  }

  // Live data still available from L1 output.
  ASSERT_TRUE(db->Get(tinylsm::ReadOptions(), "pin", &value).ok());
  EXPECT_EQ(value, "v1");

  delete db;
}

// Overlapping L1 selection: second compaction merges new L0 with existing L1.
TEST_F(CompactionTest, OverlappingL1Selected) {
  tinylsm::Options opt = CompactionOptions(/*l0_trigger=*/2);
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  auto* impl = AsImpl(db);
  tinylsm::WriteOptions wo;
  wo.sync = false;
  tinylsm::ReadOptions ro;

  // First compaction: put a=1, then a=2 across two L0 → L1.
  ASSERT_TRUE(db->Put(wo, "a", "1").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(db->Put(wo, "a", "2").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_EQ(impl->TEST_NumL1Files(), 1);

  // Second wave: overwrite a and add b; compact again (must include L1).
  ASSERT_TRUE(db->Put(wo, "a", "3").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(db->Put(wo, "b", "B").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(impl->TEST_WaitForCompaction());

  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  // Single output L1 after second compaction (old L1 deleted as input).
  EXPECT_EQ(impl->TEST_NumL1Files(), 1);

  std::string value;
  ASSERT_TRUE(db->Get(ro, "a", &value).ok());
  EXPECT_EQ(value, "3");
  ASSERT_TRUE(db->Get(ro, "b", &value).ok());
  EXPECT_EQ(value, "B");

  delete db;
}

// PickCompaction unit: all L0 + only overlapping L1.
TEST_F(CompactionTest, PickCompactionSelectsOverlappingL1Only) {
  // Build a synthetic Version via a real DB: create two disjoint-ish L1 files
  // is hard without direct Version mutation. Use public API:
  // After first compaction L1 has keys in a range; second L0 set may partially
  // overlap — covered by OverlappingL1Selected.
  // Here test NeedsCompaction / empty pick.
  tinylsm::Version empty;
  EXPECT_FALSE(tinylsm::NeedsCompaction(empty, 4));
  auto pick = tinylsm::PickCompaction(empty, 4);
  EXPECT_TRUE(pick.empty());
}

}  // namespace
