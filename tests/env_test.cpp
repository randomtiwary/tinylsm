#include "tinylsm/env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

class EnvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = tinylsm::Env::Default();
    ASSERT_NE(env_, nullptr);
    ASSERT_TRUE(env_->NewTempDir(&tmpdir_).ok()) << "NewTempDir failed";
    ASSERT_FALSE(tmpdir_.empty());
  }

  void TearDown() override {
    if (tmpdir_.empty()) {
      return;
    }
    // Best-effort cleanup of files we created.
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

  tinylsm::Env* env_ = nullptr;
  std::string tmpdir_;
};

TEST_F(EnvTest, WriteReadSequential) {
  const std::string path = Path("hello.txt");
  {
    std::unique_ptr<tinylsm::WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append("hello ").ok());
    ASSERT_TRUE(w->Append("world").ok());
    ASSERT_TRUE(w->Sync().ok());
    ASSERT_TRUE(w->Close().ok());
  }

  EXPECT_TRUE(env_->FileExists(path));

  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(path, &size).ok());
  EXPECT_EQ(size, 11u);

  std::unique_ptr<tinylsm::SequentialFile> r;
  ASSERT_TRUE(env_->NewSequentialFile(path, &r).ok());
  std::string chunk;
  ASSERT_TRUE(r->Read(5, &chunk).ok());
  EXPECT_EQ(chunk, "hello");
  ASSERT_TRUE(r->Read(100, &chunk).ok());
  EXPECT_EQ(chunk, " world");
  ASSERT_TRUE(r->Read(10, &chunk).ok());
  EXPECT_EQ(chunk, "");  // EOF
}

TEST_F(EnvTest, SequentialSkip) {
  const std::string path = Path("skip.txt");
  {
    std::unique_ptr<tinylsm::WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append("abcdefghij").ok());
    ASSERT_TRUE(w->Close().ok());
  }

  std::unique_ptr<tinylsm::SequentialFile> r;
  ASSERT_TRUE(env_->NewSequentialFile(path, &r).ok());
  ASSERT_TRUE(r->Skip(3).ok());
  std::string chunk;
  ASSERT_TRUE(r->Read(4, &chunk).ok());
  EXPECT_EQ(chunk, "defg");
}

TEST_F(EnvTest, RandomAccessPread) {
  const std::string path = Path("rand.txt");
  {
    std::unique_ptr<tinylsm::WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Append("0123456789abcdef").ok());
    ASSERT_TRUE(w->Close().ok());
  }

  std::unique_ptr<tinylsm::RandomAccessFile> r;
  ASSERT_TRUE(env_->NewRandomAccessFile(path, &r).ok());
  std::string chunk;
  ASSERT_TRUE(r->Read(4, 6, &chunk).ok());
  EXPECT_EQ(chunk, "456789");
  ASSERT_TRUE(r->Read(14, 10, &chunk).ok());
  EXPECT_EQ(chunk, "ef");  // partial at EOF
  ASSERT_TRUE(r->Read(100, 4, &chunk).ok());
  EXPECT_EQ(chunk, "");
}

TEST_F(EnvTest, RenameAndList) {
  const std::string a = Path("a.txt");
  const std::string b = Path("b.txt");
  {
    std::unique_ptr<tinylsm::WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(a, &w).ok());
    ASSERT_TRUE(w->Append("x").ok());
    ASSERT_TRUE(w->Close().ok());
  }

  ASSERT_TRUE(env_->RenameFile(a, b).ok());
  EXPECT_FALSE(env_->FileExists(a));
  EXPECT_TRUE(env_->FileExists(b));

  std::vector<std::string> children;
  ASSERT_TRUE(env_->GetChildren(tmpdir_, &children).ok());
  EXPECT_NE(std::find(children.begin(), children.end(), "b.txt"),
            children.end());
  EXPECT_EQ(std::find(children.begin(), children.end(), "a.txt"),
            children.end());
}

TEST_F(EnvTest, DeleteFile) {
  const std::string path = Path("del.txt");
  {
    std::unique_ptr<tinylsm::WritableFile> w;
    ASSERT_TRUE(env_->NewWritableFile(path, &w).ok());
    ASSERT_TRUE(w->Close().ok());
  }
  ASSERT_TRUE(env_->FileExists(path));
  ASSERT_TRUE(env_->DeleteFile(path).ok());
  EXPECT_FALSE(env_->FileExists(path));
}

TEST_F(EnvTest, LockExclusiveSameProcess) {
  const std::string lock_path = Path("LOCK");

  std::unique_ptr<tinylsm::FileLock> lock1;
  ASSERT_TRUE(env_->LockFile(lock_path, &lock1).ok());
  ASSERT_NE(lock1, nullptr);

  // Second lock on same path must fail (single-process exclusivity).
  std::unique_ptr<tinylsm::FileLock> lock2;
  tinylsm::Status s = env_->LockFile(lock_path, &lock2);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(lock2, nullptr);

  ASSERT_TRUE(env_->UnlockFile(std::move(lock1)).ok());

  // After unlock, lock succeeds again.
  ASSERT_TRUE(env_->LockFile(lock_path, &lock2).ok());
  ASSERT_NE(lock2, nullptr);
  ASSERT_TRUE(env_->UnlockFile(std::move(lock2)).ok());
}

TEST_F(EnvTest, FsyncDirAndNowMicros) {
  ASSERT_TRUE(env_->FsyncDir(tmpdir_).ok());
  const uint64_t t0 = env_->NowMicros();
  const uint64_t t1 = env_->NowMicros();
  EXPECT_GE(t1, t0);
}

TEST_F(EnvTest, MissingFileErrors) {
  const std::string missing = Path("no_such_file");
  std::unique_ptr<tinylsm::SequentialFile> seq;
  EXPECT_FALSE(env_->NewSequentialFile(missing, &seq).ok());
  std::unique_ptr<tinylsm::RandomAccessFile> rnd;
  EXPECT_FALSE(env_->NewRandomAccessFile(missing, &rnd).ok());
  uint64_t size = 0;
  EXPECT_FALSE(env_->GetFileSize(missing, &size).ok());
}

}  // namespace
