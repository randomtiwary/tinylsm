#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

class DbBasicTest : public ::testing::Test {
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

  void DestroyDB() {
    // Best-effort wipe of directory contents (for reopen scenarios).
    std::vector<std::string> children;
    if (env_->GetChildren(dbname_, &children).ok()) {
      for (const auto& name : children) {
        (void)env_->DeleteFile(dbname_ + "/" + name);
      }
    }
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

TEST_F(DbBasicTest, PutGetDelete) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  ASSERT_NE(db, nullptr);
  std::unique_ptr<tinylsm::DB> holder(db);

  tinylsm::WriteOptions wo;
  wo.sync = true;
  tinylsm::ReadOptions ro;

  ASSERT_TRUE(db->Put(wo, "hello", "world").ok());
  std::string value;
  ASSERT_TRUE(db->Get(ro, "hello", &value).ok());
  EXPECT_EQ(value, "world");

  ASSERT_TRUE(db->Delete(wo, "hello").ok());
  tinylsm::Status s = db->Get(ro, "hello", &value);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

TEST_F(DbBasicTest, Overwrite) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);

  tinylsm::WriteOptions wo;
  wo.sync = false;
  tinylsm::ReadOptions ro;

  ASSERT_TRUE(db->Put(wo, "k", "v1").ok());
  ASSERT_TRUE(db->Put(wo, "k", "v2").ok());
  std::string value;
  ASSERT_TRUE(db->Get(ro, "k", &value).ok());
  EXPECT_EQ(value, "v2");
}

TEST_F(DbBasicTest, MissingKey) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);

  tinylsm::ReadOptions ro;
  std::string value;
  tinylsm::Status s = db->Get(ro, "nope", &value);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

TEST_F(DbBasicTest, CreateIfMissingFalseFailsOnEmpty) {
  tinylsm::Options opt = DefaultOptions();
  opt.create_if_missing = false;
  tinylsm::DB* db = nullptr;
  tinylsm::Status s = tinylsm::DB::Open(opt, dbname_, &db);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsInvalidArgument() || s.IsIOError()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DbBasicTest, ErrorIfExists) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
    delete db;
  }

  tinylsm::Options opt = DefaultOptions();
  opt.create_if_missing = true;
  opt.error_if_exists = true;
  tinylsm::DB* db = nullptr;
  tinylsm::Status s = tinylsm::DB::Open(opt, dbname_, &db);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DbBasicTest, MultipleKeysAndReopenSameProcess) {
  {
    tinylsm::DB* db = nullptr;
    ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
    tinylsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, "a", "1").ok());
    ASSERT_TRUE(db->Put(wo, "b", "2").ok());
    ASSERT_TRUE(db->Put(wo, "c", "3").ok());
    delete db;
  }

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(DefaultOptions(), dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "a", &value).ok());
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(db->Get(ro, "b", &value).ok());
  EXPECT_EQ(value, "2");
  ASSERT_TRUE(db->Get(ro, "c", &value).ok());
  EXPECT_EQ(value, "3");
}

TEST_F(DbBasicTest, KeyValueSizeLimits) {
  tinylsm::Options opt = DefaultOptions();
  opt.max_key_size = 4;
  opt.max_value_size = 4;
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(opt, dbname_, &db).ok());
  std::unique_ptr<tinylsm::DB> holder(db);

  tinylsm::WriteOptions wo;
  EXPECT_TRUE(db->Put(wo, "toolong", "v").IsInvalidArgument());
  EXPECT_TRUE(db->Put(wo, "k", "toolong").IsInvalidArgument());
  ASSERT_TRUE(db->Put(wo, "k", "v").ok());
}

}  // namespace
