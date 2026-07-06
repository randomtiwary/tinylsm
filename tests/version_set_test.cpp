#include "version_set.h"

#include "internal_key.h"
#include "tinylsm/env.h"
#include "tinylsm/filename.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

class VersionSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = tinylsm::Env::Default();
    ASSERT_TRUE(env_->NewTempDir(&dbname_).ok());
  }

  void TearDown() override {
    if (dbname_.empty()) {
      return;
    }
    // Best-effort recursive-ish cleanup of files we created.
    std::vector<std::string> children;
    if (env_->GetChildren(dbname_, &children).ok()) {
      for (const auto& name : children) {
        (void)env_->DeleteFile(dbname_ + "/" + name);
      }
    }
    (void)env_->DeleteDir(dbname_);
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

TEST_F(VersionSetTest, NewDBCreatesCurrentAndManifest) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok()) << "NewDB failed";

  EXPECT_TRUE(env_->FileExists(tinylsm::CurrentFileName(dbname_)));
  EXPECT_TRUE(env_->FileExists(tinylsm::ManifestFileName(dbname_, 1)));
  EXPECT_TRUE(env_->FileExists(tinylsm::LogFileName(dbname_, 1)));

  EXPECT_EQ(vs.LogNumber(), 1u);
  EXPECT_EQ(vs.PeekNextFileNumber(), 2u);
  EXPECT_EQ(vs.LastSequence(), 0u);
  EXPECT_EQ(vs.ManifestFileNumber(), 1u);
  ASSERT_NE(vs.current(), nullptr);
  EXPECT_EQ(vs.current()->NumFiles(0), 0u);
  EXPECT_EQ(vs.current()->NumFiles(1), 0u);

  // CURRENT content is MANIFEST-000001\n (padding matches filename helper).
  std::unique_ptr<tinylsm::SequentialFile> cur;
  ASSERT_TRUE(
      env_->NewSequentialFile(tinylsm::CurrentFileName(dbname_), &cur).ok());
  std::string contents;
  ASSERT_TRUE(cur->Read(256, &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-000001\n");
}

TEST_F(VersionSetTest, NewFileNumberAllocator) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok());
  EXPECT_EQ(vs.PeekNextFileNumber(), 2u);
  EXPECT_EQ(vs.NewFileNumber(), 2u);
  EXPECT_EQ(vs.NewFileNumber(), 3u);
  EXPECT_EQ(vs.PeekNextFileNumber(), 4u);
}

TEST_F(VersionSetTest, LogAndApplyAddFileThenRecover) {
  {
    tinylsm::VersionSet vs(dbname_, env_);
    ASSERT_TRUE(vs.NewDB().ok());

    const uint64_t file_no = vs.NewFileNumber();  // 2
    const std::string smallest =
        tinylsm::EncodeInternalKey("a", 1, tinylsm::kTypeValue);
    const std::string largest =
        tinylsm::EncodeInternalKey("b", 2, tinylsm::kTypeValue);

    tinylsm::VersionEdit edit;
    edit.AddFile(/*level=*/0, file_no, /*size=*/1234, smallest, largest);
    edit.SetLogNumber(2);
    edit.SetNextFileNumber(vs.PeekNextFileNumber());
    edit.SetLastSequence(10);
    ASSERT_TRUE(vs.LogAndApply(&edit).ok());

    EXPECT_EQ(vs.current()->NumFiles(0), 1u);
    auto f = vs.current()->FindFile(0, file_no);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->file_size, 1234u);
    EXPECT_EQ(f->smallest, smallest);
    EXPECT_EQ(vs.LastSequence(), 10u);
    EXPECT_EQ(vs.LogNumber(), 2u);
  }

  // Fresh VersionSet recovers from disk.
  tinylsm::VersionSet recovered(dbname_, env_);
  ASSERT_TRUE(recovered.Recover().ok());
  EXPECT_EQ(recovered.current()->NumFiles(0), 1u);
  auto f = recovered.current()->FindFile(0, 2);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->file_size, 1234u);
  EXPECT_EQ(recovered.LastSequence(), 10u);
  EXPECT_EQ(recovered.LogNumber(), 2u);
  EXPECT_GE(recovered.PeekNextFileNumber(), 3u);
}

TEST_F(VersionSetTest, DeleteFileRemovesOnRecover) {
  {
    tinylsm::VersionSet vs(dbname_, env_);
    ASSERT_TRUE(vs.NewDB().ok());

    const uint64_t f1 = vs.NewFileNumber();
    const uint64_t f2 = vs.NewFileNumber();
    const std::string k1 =
        tinylsm::EncodeInternalKey("k1", 1, tinylsm::kTypeValue);
    const std::string k2 =
        tinylsm::EncodeInternalKey("k2", 2, tinylsm::kTypeValue);

    tinylsm::VersionEdit add;
    add.AddFile(0, f1, 100, k1, k1);
    add.AddFile(0, f2, 200, k2, k2);
    add.SetNextFileNumber(vs.PeekNextFileNumber());
    add.SetLastSequence(2);
    ASSERT_TRUE(vs.LogAndApply(&add).ok());
    EXPECT_EQ(vs.current()->NumFiles(0), 2u);

    tinylsm::VersionEdit del;
    del.DeleteFile(0, f1);
    del.SetNextFileNumber(vs.PeekNextFileNumber());
    ASSERT_TRUE(vs.LogAndApply(&del).ok());
    EXPECT_EQ(vs.current()->NumFiles(0), 1u);
    EXPECT_EQ(vs.current()->FindFile(0, f1), nullptr);
    EXPECT_NE(vs.current()->FindFile(0, f2), nullptr);
  }

  tinylsm::VersionSet recovered(dbname_, env_);
  ASSERT_TRUE(recovered.Recover().ok());
  EXPECT_EQ(recovered.current()->NumFiles(0), 1u);
  EXPECT_EQ(recovered.current()->FindFile(0, /*f1=*/2), nullptr);
  EXPECT_NE(recovered.current()->FindFile(0, /*f2=*/3), nullptr);
}

TEST_F(VersionSetTest, AtomicCurrentBasic) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok());

  // After NewDB, CURRENT names MANIFEST-000001 and no leftover CURRENT.tmp.
  EXPECT_TRUE(env_->FileExists(tinylsm::CurrentFileName(dbname_)));
  EXPECT_FALSE(env_->FileExists(tinylsm::CurrentFileName(dbname_) + ".tmp"));

  std::unique_ptr<tinylsm::SequentialFile> cur;
  ASSERT_TRUE(
      env_->NewSequentialFile(tinylsm::CurrentFileName(dbname_), &cur).ok());
  std::string contents;
  ASSERT_TRUE(cur->Read(256, &contents).ok());
  // Exactly one line ending with newline, basename only.
  EXPECT_EQ(contents.find('\n'), contents.size() - 1);
  EXPECT_EQ(contents.substr(0, contents.size() - 1), "MANIFEST-000001");
}

TEST_F(VersionSetTest, SetLastSequence) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok());
  vs.SetLastSequence(42);
  EXPECT_EQ(vs.LastSequence(), 42u);
}

TEST_F(VersionSetTest, CowKeepsOldVersionAlive) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok());

  const uint64_t f1 = vs.NewFileNumber();
  const std::string k =
      tinylsm::EncodeInternalKey("x", 1, tinylsm::kTypeValue);
  tinylsm::VersionEdit add;
  add.AddFile(0, f1, 10, k, k);
  add.SetNextFileNumber(vs.PeekNextFileNumber());
  ASSERT_TRUE(vs.LogAndApply(&add).ok());

  auto old = vs.current();
  EXPECT_EQ(old->NumFiles(0), 1u);

  tinylsm::VersionEdit del;
  del.DeleteFile(0, f1);
  del.SetNextFileNumber(vs.PeekNextFileNumber());
  ASSERT_TRUE(vs.LogAndApply(&del).ok());

  // Published version is empty; held old snapshot still lists the file.
  EXPECT_EQ(vs.current()->NumFiles(0), 0u);
  EXPECT_EQ(old->NumFiles(0), 1u);
  EXPECT_NE(old->FindFile(0, f1), nullptr);
}

TEST_F(VersionSetTest, L1AddFile) {
  tinylsm::VersionSet vs(dbname_, env_);
  ASSERT_TRUE(vs.NewDB().ok());
  const uint64_t f = vs.NewFileNumber();
  const std::string lo =
      tinylsm::EncodeInternalKey("m", 1, tinylsm::kTypeValue);
  const std::string hi =
      tinylsm::EncodeInternalKey("z", 1, tinylsm::kTypeValue);
  tinylsm::VersionEdit edit;
  edit.AddFile(1, f, 50, lo, hi);
  edit.SetNextFileNumber(vs.PeekNextFileNumber());
  ASSERT_TRUE(vs.LogAndApply(&edit).ok());
  EXPECT_EQ(vs.current()->NumFiles(1), 1u);
  EXPECT_EQ(vs.current()->NumFiles(0), 0u);
}

}  // namespace
