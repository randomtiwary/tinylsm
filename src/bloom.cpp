#include "bloom.h"

#include <algorithm>
#include <cmath>

namespace tinylsm {

// Same family as LevelDB's util/hash.cc (seed fixed for on-disk stability).
uint32_t BloomHash(std::string_view key) {
  const uint32_t seed = 0xbc9f1d34;
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  const auto n = static_cast<uint32_t>(key.size());
  uint32_t h = seed ^ (n * m);

  const char* data = key.data();
  const char* limit = data + n;
  while (data + 4 <= limit) {
    uint32_t w = static_cast<uint8_t>(data[0]) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint8_t>(data[0]);
      h *= m;
      h ^= (h >> r);
      break;
  }
  return h;
}

namespace {

// Round bits_per_key * ln(2) to choose probe count (LevelDB heuristic).
int ProbesForBitsPerKey(int bits_per_key) {
  // k = bits_per_key * ln(2) ≈ bits_per_key * 0.69
  int k = static_cast<int>(std::lround(bits_per_key * 0.69));
  if (k < 1) {
    k = 1;
  }
  if (k > 30) {
    k = 30;
  }
  return k;
}

}  // namespace

std::string CreateFilter(const std::vector<std::string_view>& keys,
                         int bits_per_key) {
  if (bits_per_key <= 0 || keys.empty()) {
    return std::string();
  }

  // bits = n * bits_per_key, at least 64 so tiny sets still have room.
  size_t bits = keys.size() * static_cast<size_t>(bits_per_key);
  if (bits < 64) {
    bits = 64;
  }
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const int k = ProbesForBitsPerKey(bits_per_key);

  std::string filter(bytes, '\0');
  filter.push_back(static_cast<char>(k));

  for (std::string_view key : keys) {
    uint32_t h = BloomHash(key);
    // Double hashing: rotate for delta (LevelDB).
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int i = 0; i < k; ++i) {
      const uint32_t bitpos = h % static_cast<uint32_t>(bits);
      filter[bitpos / 8] =
          static_cast<char>(static_cast<uint8_t>(filter[bitpos / 8]) |
                            (1u << (bitpos % 8)));
      h += delta;
    }
  }
  return filter;
}

bool KeyMayMatch(std::string_view key, std::string_view filter) {
  const size_t len = filter.size();
  if (len < 2) {
    // Missing/corrupt filter: do not claim "definitely absent".
    return true;
  }
  const int k = static_cast<uint8_t>(filter[len - 1]);
  if (k > 30) {
    // Reserved / new encoding — be conservative.
    return true;
  }
  if (k == 0) {
    // Potentially empty deliberately; treat as match-all.
    return true;
  }

  const size_t bits = (len - 1) * 8;
  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int i = 0; i < k; ++i) {
    const uint32_t bitpos = h % static_cast<uint32_t>(bits);
    if ((static_cast<uint8_t>(filter[bitpos / 8]) & (1u << (bitpos % 8))) ==
        0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace tinylsm
