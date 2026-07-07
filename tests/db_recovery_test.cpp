#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
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

  void AppendBytesToFile(const std::string& path, const std::string& bytes) {
    // Read whole file, rewrite with append (no appendable needed beyond Env API).
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
    // Destructor releases LOCK and closes WAL — no separate clean flush.
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
  // Append a partial frame (length claims more bytes than present).
  // fixed32 length = 100, fixed32 fake crc, only 3 payload bytes.
  std::string torn;
  // length = 100
  torn.push_back(static_cast<char>(100));
  torn.push_back(0);
  torn.push_back(0);
  torn.push_back(0);
  // crc placeholder
  torn.push_back(1);
  torn.push_back(2);
  torn.push_back(3);
  torn.push_back(4);
  // incomplete payload
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

  // After release, Open succeeds again.
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db2).ok());
  delete db2;
}

}  // namespace
