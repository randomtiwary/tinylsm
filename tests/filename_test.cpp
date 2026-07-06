#include "tinylsm/filename.h"

#include <gtest/gtest.h>

namespace {

TEST(FilenameTest, LogFileName) {
  EXPECT_EQ(tinylsm::LogFileName("/tmp/db", 1), "/tmp/db/000001.log");
  EXPECT_EQ(tinylsm::LogFileName("/tmp/db", 1234567),
            "/tmp/db/1234567.log");  // wider than 6 still ok
}

TEST(FilenameTest, TableFileName) {
  EXPECT_EQ(tinylsm::TableFileName("mydb", 42), "mydb/000042.sst");
}

TEST(FilenameTest, TableTempFileName) {
  EXPECT_EQ(tinylsm::TableTempFileName("mydb", 7), "mydb/000007.sst.tmp");
}

TEST(FilenameTest, ManifestFileName) {
  EXPECT_EQ(tinylsm::ManifestFileName("/data/db", 3),
            "/data/db/MANIFEST-000003");
}

TEST(FilenameTest, CurrentAndLock) {
  EXPECT_EQ(tinylsm::CurrentFileName("/db"), "/db/CURRENT");
  EXPECT_EQ(tinylsm::LockFileName("/db"), "/db/LOCK");
}

TEST(FilenameTest, TempFileName) {
  EXPECT_EQ(tinylsm::TempFileName("db", 9), "db/000009.dbtmp");
}

TEST(FilenameTest, TrailingSlashDBname) {
  // Avoid double slash when dbname already ends with '/'.
  EXPECT_EQ(tinylsm::LogFileName("/tmp/db/", 1), "/tmp/db/000001.log");
  EXPECT_EQ(tinylsm::CurrentFileName("/tmp/db/"), "/tmp/db/CURRENT");
}

TEST(FilenameTest, Zero) {
  EXPECT_EQ(tinylsm::LogFileName("x", 0), "x/000000.log");
}

}  // namespace
