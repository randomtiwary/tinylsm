#include "db_impl.h"

#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class DbRecoveryTest : public ::testing::Test {
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

  tinylsm::Options DefaultOptions() const {
    tinylsm::Options opt;
    opt.create_if_missing = true;
    opt.error_if_exists = false;
    opt.env = env_;
    return opt;
  }

  tinylsm::Options SmallBufferOptions(size_t write_buffer = 256) const {
    tinylsm::Options opt = DefaultOptions();
    opt.write_buffer_size = write_buffer;
    return opt;
  }

  static tinylsm::DBImpl* AsImpl(tinylsm::DB* db) {
    return static_cast<tinylsm::DBImpl*>(db);
  }

  // Find the sole (or highest) *.log path under dbname_.
  std::string FindHighestLogPath() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).ok());
    uint64_t best = 0;
    bool found = false;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
        const std::string num = name.substr(0, name.size() - 4);
        char* end = nullptr;
        const unsigned long long v = std::strtoull(num.c_str(), &end, 10);
        if (end != num.c_str() && *end == '\0') {
          if (!found || v > best) {
            best = static_cast<uint64_t>(v);
            found = true;
          }
        }
      }
    }
    EXPECT_TRUE(found);
    return tinylsm::LogFileName(dbname_, best);
  }

  std::vector<uint64_t> ListLogNumbers() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).ok());
    std::vector<uint64_t> logs;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
        const std::string num = name.substr(0, name.size() - 4);
        char* end = nullptr;
        const unsigned long long v = std::strtoull(num.c_str(), &end, 10);
        if (end != num.c_str() && *end == '\0') {
          logs.push_back(static_cast<uint64_t>(v));
        }
      }
    }
    return logs;
  }

  std::vector<uint64_t> ListSstNumbers() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).ok());
    std::vector<uint64_t> out;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sst") == 0) {
        const std::string num = name.substr(0, name.size() - 4);
        char* end = nullptr;
        const unsigned long long v = std::strtoull(num.c_str(), &end, 10);
        if (end != num.c_str() && *end == '\0') {
          out.push_back(static_cast<uint64_t>(v));
        }
      }
    }
    return out;
  }

  void AppendBytesToFile(const std::string& path, const std::string& bytes) {
    std::string existing;
    {
      std::unique_ptr<tinylsm::SequentialFile> in;
      ASSERT_TRUE(env_->NewSequentialFile(path, &in).ok());
      for (;;) {
        std::string chunk;
        ASSERT_TRUE(in->Read(4096, &chunk).ok());
        if (chunk.empty()) {
          break;
        }
        existing.append(chunk);
      }
    }
    existing.append(bytes);
    std::unique_ptr<tinylsm::WritableFile> out;
    ASSERT_TRUE(env_->NewWritableFile(path, &out).ok());
    ASSERT_TRUE(out->Append(existing).ok());
    ASSERT_TRUE(out->Sync().ok());
    ASSERT_TRUE(out->Close().ok());
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

// R1: unclean reopen after synced puts — data survives close + reopen.
TEST_F(DbRecoveryTest, R1_UncleanReopenAfterSyncedPuts) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "durable", "yes").ok());
    ASSERT_TRUE(db->Put(wo, "also", "present").ok());
    ASSERT_TRUE(db->Delete(wo, "also").ok());
    delete db;
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "durable", &value).ok());
  EXPECT_EQ(value, "yes");
  tinylsm::Status s = db->Get(ro, "also", &value);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

// R2: torn WAL tail — incomplete final frame is ignored; prior records apply.
TEST_F(DbRecoveryTest, R2_TornWalTail) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "k1", "v1").ok());
    ASSERT_TRUE(db->Put(wo, "k2", "v2").ok());
    delete db;
  }

  const std::string log_path = FindHighestLogPath();
  std::string torn;
  torn.push_back(static_cast<char>(100));
  torn.push_back(0);
  torn.push_back(0);
  torn.push_back(0);
  torn.push_back(1);
  torn.push_back(2);
  torn.push_back(3);
  torn.push_back(4);
  torn.append("xxx");
  AppendBytesToFile(log_path, torn);

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok())
      << "reopen after torn tail must succeed";
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "k1", &value).ok());
  EXPECT_EQ(value, "v1");
  ASSERT_TRUE(db->Get(ro, "k2", &value).ok());
  EXPECT_EQ(value, "v2");
}

// R7: second Open while first still open fails with IOError (LOCK).
TEST_F(DbRecoveryTest, R7_SecondOpenWhileLockedFails) {
  tinylsm::DB* db1 = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db1).ok());
  ASSERT_NE(db1, nullptr);

  tinylsm::DB* db2 = nullptr;
  tinylsm::Status s = tinylsm::DB::Open(DefaultOptions(), dbname_, &db2);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(db2, nullptr);

  delete db1;

  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db2).ok());
  delete db2;
}

// R3: freeze then crash before flush apply — multi-log replay restores data.
TEST_F(DbRecoveryTest, R3_FreezeCrashBeforeFlushApply) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(300), dbname_, &db).ok());
    auto* impl = AsImpl(db);

    // Inject fail after durable SST write, before LogAndApply.
    impl->TEST_SetFailBeforeFlushApply(true);

    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "imm_key", "from_imm").ok());
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    // Active mem after freeze.
    ASSERT_TRUE(db->Put(wo, "active_key", "from_active").ok());

    // Wait for the failed flush attempt (clears imm; leaves orphan + multi-log).
    for (int i = 0; i < 200 && impl->TEST_HasImm(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Manifest must NOT yet include flushed data as sole source — log_number
    // should still be the original (typically 1) because apply was skipped.
    EXPECT_EQ(impl->TEST_ManifestLogNumber(), 1u);

    // Multi-log present on disk.
    auto logs = ListLogNumbers();
    EXPECT_GE(logs.size(), 2u);

    // Simulate crash (do not finish clean shutdown flush).
    impl->TEST_SimulateCrash();
    delete db;
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "imm_key", &value).ok()) << "imm data via multi-log";
  EXPECT_EQ(value, "from_imm");
  ASSERT_TRUE(db->Get(ro, "active_key", &value).ok()) << "active data via multi-log";
  EXPECT_EQ(value, "from_active");
}

// R4: orphan SST (file on disk not in manifest) is ignored on recovery.
TEST_F(DbRecoveryTest, R4_OrphanSstIgnored) {
  // Create a normal DB with one key, then plant an orphan SST number.
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "real", "data").ok());
    delete db;
  }

  // Write a junk file with a high unused number that looks like an SST.
  // Open must ignore it (not in manifest) and still serve real from WAL.
  const std::string orphan = tinylsm::TableFileName(dbname_, 999999);
  {
    std::unique_ptr<tinylsm::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(orphan, &f).ok());
    // Not a valid SST — if Open tried to open all *.sst it would fail.
    ASSERT_TRUE(f->Append("not-a-valid-sst").ok());
    ASSERT_TRUE(f->Close().ok());
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok())
      << "orphan invalid SST must be ignored";
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "real", &value).ok());
  EXPECT_EQ(value, "data");
}

// R5: after flush apply, active-log keys still present on reopen (always-apply).
TEST_F(DbRecoveryTest, R5_AfterFlushActiveKeysSurviveReopen) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(300), dbname_, &db).ok());
    auto* impl = AsImpl(db);
    tinylsm::WriteOptions wo;
    wo.sync = true;

    ASSERT_TRUE(db->Put(wo, "flushed", "in_sst").ok());
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    ASSERT_TRUE(impl->TEST_WaitForFlush());
    EXPECT_GE(impl->TEST_NumL0Files(), 1);

    // Keys only in active mem/log after flush.
    ASSERT_TRUE(db->Put(wo, "active_only", "still_here").ok());
    delete db;
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "flushed", &value).ok());
  EXPECT_EQ(value, "in_sst");
  ASSERT_TRUE(db->Get(ro, "active_only", &value).ok())
      << "R0b always-apply: active-log keys must survive reopen";
  EXPECT_EQ(value, "still_here");
}

// R5b: flush imm while active mem non-empty → reopen keeps active-only keys.
TEST_F(DbRecoveryTest, R5b_FlushImmWhileActiveNonEmpty) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(300), dbname_, &db).ok());
    auto* impl = AsImpl(db);
    tinylsm::WriteOptions wo;
    wo.sync = true;

    ASSERT_TRUE(db->Put(wo, "A_imm", "A").ok());
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    // Do not wait yet — write to active while imm pending, then wait.
    ASSERT_TRUE(db->Put(wo, "B_active", "B").ok());
    ASSERT_TRUE(impl->TEST_WaitForFlush());

    // After flush, B must still be visible (active mem).
    tinylsm::ReadOptions ro;
    std::string value;
    ASSERT_TRUE(db->Get(ro, "A_imm", &value).ok());
    EXPECT_EQ(value, "A");
    ASSERT_TRUE(db->Get(ro, "B_active", &value).ok());
    EXPECT_EQ(value, "B");
    delete db;
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "A_imm", &value).ok());
  EXPECT_EQ(value, "A");
  ASSERT_TRUE(db->Get(ro, "B_active", &value).ok());
  EXPECT_EQ(value, "B");
}

// R8: next_file_number no reuse after freeze crash path.
TEST_F(DbRecoveryTest, R8_NextFileNumberNoReuseAfterFreezeCrash) {
  uint64_t log_created_at_freeze = 0;
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(300), dbname_, &db).ok());
    auto* impl = AsImpl(db);
    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "x", "1").ok());

    const uint64_t before = impl->TEST_PeekNextFileNumber();
    ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
    log_created_at_freeze = impl->TEST_CurrentLogNumber();
    EXPECT_GE(log_created_at_freeze, before);
    // Crash before flush apply so freeze-created log is only on disk, not in
    // manifest next_file_number high-water (unless bump-from-dir works).
    impl->TEST_SetFailBeforeFlushApply(true);
    // Wait for fail path to run (or crash immediately).
    for (int i = 0; i < 100 && impl->TEST_HasImm(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    impl->TEST_SimulateCrash();
    delete db;
  }

  // On-disk max must include freeze log; Open bumps allocator (R0c).
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  auto* impl = AsImpl(db);
  const uint64_t peek = impl->TEST_PeekNextFileNumber();
  EXPECT_GT(peek, log_created_at_freeze)
      << "allocator must be past freeze-created file numbers";

  // Allocating a new number must not collide with existing log paths.
  tinylsm::WriteOptions wo;
  wo.sync = true;
  // Force another freeze to allocate more numbers.
  ASSERT_TRUE(db->Put(wo, "y", "2").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  auto logs = ListLogNumbers();
  auto ssts = ListSstNumbers();
  // All basenames unique — trivial if no crash; check no two files share number.
  std::vector<uint64_t> all = logs;
  all.insert(all.end(), ssts.begin(), ssts.end());
  std::sort(all.begin(), all.end());
  for (size_t i = 1; i < all.size(); ++i) {
    EXPECT_NE(all[i], all[i - 1]) << "file number reused on disk";
  }

  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "x", &value).ok());
  EXPECT_EQ(value, "1");
  delete db;
}

// R9: large WAL replay schedules post-open flush (oversized mem).
TEST_F(DbRecoveryTest, R9_OversizedWalReplaySchedulesFlush) {
  // Build a large WAL without flushing (large write_buffer), then reopen with
  // small write_buffer so recovery freezes and schedules flush.
  {
    tinylsm::DB* db = nullptr;
    tinylsm::Options big = DefaultOptions();
    big.write_buffer_size = 64 * 1024 * 1024;  // avoid flush while filling
    ASSERT_TRUE(tinylsm::DB::Open(big, dbname_, &db).ok());
    tinylsm::WriteOptions wo;
    wo.sync = true;
    const std::string payload(200, 'z');
    for (int i = 0; i < 40; ++i) {
      ASSERT_TRUE(db->Put(wo, "bulk" + std::to_string(i), payload).ok());
    }
    delete db;
  }

  tinylsm::DB* db = nullptr;
  tinylsm::Options small = SmallBufferOptions(500);
  ASSERT_TRUE(tinylsm::DB::Open(small, dbname_, &db).ok());
  auto* impl = AsImpl(db);

  // Post-open freeze should have scheduled (or completed) a flush.
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  // After flush, mem should not remain at full replay size forever.
  // L0 should have received the recovered data.
  EXPECT_GE(impl->TEST_NumL0Files(), 1);

  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "bulk0", &value).ok());
  EXPECT_EQ(value, std::string(200, 'z'));
  ASSERT_TRUE(db->Get(ro, "bulk39", &value).ok());
  delete db;
}

}  // namespace
