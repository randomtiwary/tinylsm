#include "db_impl.h"

#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class DbFlushTest : public ::testing::Test {
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

  tinylsm::Options SmallBufferOptions(size_t write_buffer = 256) const {
    tinylsm::Options opt;
    opt.create_if_missing = true;
    opt.error_if_exists = false;
    opt.env = env_;
    opt.write_buffer_size = write_buffer;
    return opt;
  }

  static tinylsm::DBImpl* AsImpl(tinylsm::DB* db) {
    return static_cast<tinylsm::DBImpl*>(db);
  }

  // Count *.sst files in the DB directory.
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

  int CountLogFiles() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).ok());
    int n = 0;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
        ++n;
      }
    }
    return n;
  }

  // Write enough payload that ApproximateMemoryUsage crosses write_buffer_size.
  void FillUntilFreeze(tinylsm::DB* db, const std::string& key_prefix,
                       int* keys_written) {
    tinylsm::WriteOptions wo;
    wo.sync = false;
    auto* impl = AsImpl(db);
    *keys_written = 0;
    // Values large enough to fill a small buffer quickly.
    const std::string payload(64, 'x');
    for (int i = 0; i < 10000; ++i) {
      const std::string key = key_prefix + std::to_string(i);
      ASSERT_TRUE(db->Put(wo, key, payload).ok()) << "put " << key;
      ++(*keys_written);
      if (impl->TEST_HasImm() || impl->TEST_NumL0Files() > 0) {
        // Freeze happened (imm pending or already flushed).
        break;
      }
      // Also detect post-flush: mem usage dropped after freeze+new mem.
      if (i > 0 && impl->TEST_MemApproximateMemoryUsage() < 64) {
        break;
      }
    }
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

// Basic: writes that exceed write_buffer produce an L0 file; data readable.
TEST_F(DbFlushTest, FlushProducesL0AndReadable) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(512), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  int n = 0;
  FillUntilFreeze(db, "k", &n);
  ASSERT_GT(n, 0);
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  EXPECT_GE(impl->TEST_NumL0Files(), 1);
  EXPECT_GE(CountSstFiles(), 1);

  tinylsm::ReadOptions ro;
  std::string value;
  // First key written must still be present (in SST or mem).
  ASSERT_TRUE(db->Get(ro, "k0", &value).ok());
  EXPECT_EQ(value, std::string(64, 'x'));
}

// C2: write stall when imm pending unblocks after flush.
TEST_F(DbFlushTest, C2_WriteStallUnblocksAfterFlush) {
  tinylsm::DB* db = nullptr;
  // Tiny buffer so two freezes interact with stall easily.
  ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(200), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  tinylsm::WriteOptions wo;
  wo.sync = false;
  const std::string payload(80, 'y');

  // First batch: fill and freeze; keep BG busy by not waiting first.
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(db->Put(wo, "a" + std::to_string(i), payload).ok());
  }
  // Ensure at least one freeze occurred.
  // Fill active mem while imm may still be flushing — stall path.
  std::atomic<bool> writer_done{false};
  std::atomic<bool> writer_ok{false};
  std::thread stall_writer([&]() {
    // This may block in MakeRoomForWrite until flush clears imm.
    for (int i = 0; i < 80; ++i) {
      tinylsm::Status s = db->Put(wo, "b" + std::to_string(i), payload);
      if (!s.ok()) {
        writer_ok = false;
        writer_done = true;
        return;
      }
    }
    writer_ok = true;
    writer_done = true;
  });

  // Give the writer a chance to stall, then ensure flush progress.
  for (int i = 0; i < 200 && !writer_done.load(); ++i) {
    (void)impl->TEST_WaitForFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  stall_writer.join();
  EXPECT_TRUE(writer_done.load());
  EXPECT_TRUE(writer_ok.load());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "a0", &value).ok());
  ASSERT_TRUE(db->Get(ro, "b0", &value).ok());
}

// Get after delete survives flush — no resurrection from SST.
TEST_F(DbFlushTest, DeleteSurvivesFlushNoResurrection) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(400), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  tinylsm::WriteOptions wo;
  wo.sync = true;
  ASSERT_TRUE(db->Put(wo, "victim", "alive").ok());
  // Force into SST.
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  EXPECT_GE(impl->TEST_NumL0Files(), 1);

  // Delete must hide SST value even after another flush of the tombstone.
  ASSERT_TRUE(db->Delete(wo, "victim").ok());
  tinylsm::ReadOptions ro;
  std::string value;
  EXPECT_TRUE(db->Get(ro, "victim", &value).IsNotFound());

  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  EXPECT_TRUE(db->Get(ro, "victim", &value).IsNotFound())
      << "delete must not resurrect from older SST after flush";
}

// Delete in mem while older value only in SST (never flushed tombstone yet).
TEST_F(DbFlushTest, DeleteInMemHidesSstValue) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(SmallBufferOptions(400), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  tinylsm::WriteOptions wo;
  wo.sync = true;
  ASSERT_TRUE(db->Put(wo, "k", "old").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  ASSERT_TRUE(db->Delete(wo, "k").ok());
  tinylsm::ReadOptions ro;
  std::string value;
  // Blocker from PR12: must not fall through to SST.
  EXPECT_TRUE(db->Get(ro, "k", &value).IsNotFound());
}

}  // namespace
