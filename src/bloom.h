#pragma once

// Educational Bloom filter for SST user keys (docs/format.md §5 / design §8).
// Filter block_contents: bitmap bytes || u8 num_probes (LevelDB-style).
// No false negatives; false positives expected at ~1% with bits_per_key=10.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

// Default bits/key when bloom is enabled via options (typical educational value).
constexpr int kDefaultBloomBitsPerKey = 10;

// Hash used by the bloom filter (murmur-ish; fixed seed for determinism).
uint32_t BloomHash(std::string_view key);

// Build a filter over the given user keys.
// bits_per_key <= 0 yields an empty string (caller should not write a block).
// Result layout: [bitmap...][u8 k] where k is the number of double-hash probes.
std::string CreateFilter(const std::vector<std::string_view>& keys,
                         int bits_per_key);

// True if key may be present. False means definitely not present.
// Empty / truncated filters return true (conservative: do not skip reads).
bool KeyMayMatch(std::string_view key, std::string_view filter);

}  // namespace tinylsm
