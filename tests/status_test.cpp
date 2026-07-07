#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(StatusTest, DefaultIsOk) {
  tinylsm::Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, StaticOk) {
  tinylsm::Status s = tinylsm::Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, NotFound) {
  tinylsm::Status s = tinylsm::Status::NotFound("missing key");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_EQ(s.ToString(), "NotFound: missing key");
}

TEST(StatusTest, Corruption) {
  tinylsm::Status s = tinylsm::Status::Corruption("bad crc");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_EQ(s.ToString(), "Corruption: bad crc");
}

TEST(StatusTest, IOError) {
  tinylsm::Status s = tinylsm::Status::IOError("disk full");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(s.ToString(), "IOError: disk full");
}

TEST(StatusTest, InvalidArgument) {
  tinylsm::Status s = tinylsm::Status::InvalidArgument("key too large");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsInvalidArgument());
  EXPECT_EQ(s.ToString(), "InvalidArgument: key too large");
}

TEST(StatusTest, EmptyMessage) {
  tinylsm::Status s = tinylsm::Status::NotFound("");
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.ToString(), "NotFound");
}

TEST(StatusTest, CopyAndAssign) {
  tinylsm::Status a = tinylsm::Status::IOError("e");
  tinylsm::Status b = a;
  EXPECT_TRUE(b.IsIOError());
  EXPECT_EQ(b.ToString(), "IOError: e");

  tinylsm::Status c;
  c = a;
  EXPECT_TRUE(c.IsIOError());
}

TEST(StatusTest, Move) {
  tinylsm::Status a = tinylsm::Status::Corruption("c");
  tinylsm::Status b = std::move(a);
  EXPECT_TRUE(b.IsCorruption());
  EXPECT_EQ(b.ToString(), "Corruption: c");
}

}  // namespace
