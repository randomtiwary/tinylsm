#include "tinylsm/version.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(Smoke, VersionIsNonEmpty) {
  const char* version = tinylsm::Version();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}
