#include "bloom.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using tinylsm::CreateFilter;
using tinylsm::KeyMayMatch;
using tinylsm::kDefaultBloomBitsPerKey;

TEST(BloomTest, EmptyKeysProducesEmptyFilter) {
  std::vector<std::string_view> keys;
  EXPECT_TRUE(CreateFilter(keys, kDefaultBloomBitsPerKey).empty());
  EXPECT_TRUE(CreateFilter({"a"}, /*bits_per_key=*/0).empty());
  EXPECT_TRUE(CreateFilter({"a"}, /*bits_per_key=*/-1).empty());
}

TEST(BloomTest, NoFalseNegatives) {
  // Keys that were added must always KeyMayMatch.
  std::vector<std::string> owned;
  for (int i = 0; i < 200; ++i) {
    owned.push_back("key-" + std::to_string(i));
  }
  std::vector<std::string_view> keys;
  keys.reserve(owned.size());
  for (const auto& k : owned) {
    keys.emplace_back(k);
  }

  const std::string filter = CreateFilter(keys, kDefaultBloomBitsPerKey);
  ASSERT_FALSE(filter.empty());

  for (const auto& k : owned) {
    EXPECT_TRUE(KeyMayMatch(k, filter)) << "false negative for " << k;
  }
}

TEST(BloomTest, DistinctKeysNoFalseNegativesVariedLengths) {
  std::vector<std::string> owned = {
      "", "a", "ab", "abc", "hello", std::string(64, 'x'), "\0\1\2",
      std::string("bin\0ary", 7),
  };
  std::vector<std::string_view> keys;
  for (const auto& k : owned) {
    keys.emplace_back(k);
  }
  const std::string filter = CreateFilter(keys, /*bits_per_key=*/10);
  ASSERT_FALSE(filter.empty());
  for (const auto& k : owned) {
    EXPECT_TRUE(KeyMayMatch(k, filter));
  }
}

TEST(BloomTest, FalsePositiveRateSmoke) {
  // Not a strict statistical test — just smoke that FP rate is well below 50%
  // for bits_per_key=10 over a modest key set.
  std::vector<std::string> present;
  for (int i = 0; i < 1000; ++i) {
    present.push_back("present-" + std::to_string(i));
  }
  std::vector<std::string_view> keys;
  for (const auto& k : present) {
    keys.emplace_back(k);
  }
  const std::string filter = CreateFilter(keys, /*bits_per_key=*/10);
  ASSERT_FALSE(filter.empty());

  int fp = 0;
  const int trials = 1000;
  for (int i = 0; i < trials; ++i) {
    const std::string miss = "absent-" + std::to_string(i);
    if (KeyMayMatch(miss, filter)) {
      ++fp;
    }
  }
  // Expected FP ≈ 1% at bits_per_key=10; allow generous headroom for noise.
  EXPECT_LT(fp, trials / 5) << "fp=" << fp << " / " << trials;
}

TEST(BloomTest, EmptyOrCorruptFilterIsConservative) {
  // Must not claim "definitely absent" on unusable filters.
  EXPECT_TRUE(KeyMayMatch("anything", ""));
  EXPECT_TRUE(KeyMayMatch("anything", std::string("\x01", 1)));  // too short
}

TEST(BloomTest, DefinitelyAbsentOftenReportsFalse) {
  const std::string filter =
      CreateFilter(std::vector<std::string_view>{"only-this-key"}, 10);
  ASSERT_FALSE(filter.empty());
  EXPECT_TRUE(KeyMayMatch("only-this-key", filter));
  // Many random misses should include at least one definitive absence.
  int definitive_miss = 0;
  for (int i = 0; i < 100; ++i) {
    if (!KeyMayMatch("other-" + std::to_string(i), filter)) {
      ++definitive_miss;
    }
  }
  EXPECT_GT(definitive_miss, 0);
}

}  // namespace
