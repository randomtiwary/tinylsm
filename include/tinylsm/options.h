#pragma once

// Public open / read / write options for TinyLSM.

#include "tinylsm/env.h"

#include <cstddef>
#include <cstdint>

namespace tinylsm {

struct Options {
  // MemTable approximate flush threshold (bytes). Flush lands in a later PR.
  size_t write_buffer_size = 4 * 1024 * 1024;

  // SST data block target size (bytes).
  size_t block_size = 4 * 1024;

  // Reject Put/Delete when key or value exceeds these limits.
  size_t max_key_size = 1 * 1024 * 1024;
  size_t max_value_size = 1 * 1024 * 1024;

  // L0 file count that triggers compaction (used by later PRs).
  int l0_compaction_trigger = 4;

  // If true, create a new DB when CURRENT is missing.
  bool create_if_missing = true;

  // If true, fail Open when a DB already exists (CURRENT present).
  bool error_if_exists = false;

  // Default durability policy for educational mode (WriteOptions.sync default).
  bool sync_writes = true;

  // Bloom filter bits per user key in SST filter blocks.
  // 0 = disabled (default; filter_handle stays 0,0). Typical enable value: 10.
  int bloom_bits_per_key = 0;

  // Filesystem abstraction; nullptr means Env::Default().
  Env* env = nullptr;
};

struct ReadOptions {
  // Reserved for post-v0.1 snapshot support. Ignored in v1.
  // Get observes data at last_sequence_ sampled at the start of Get (under mutex).
};

struct WriteOptions {
  // If true, fsync/fdatasync the WAL before Put/Delete returns.
  bool sync = true;
};

}  // namespace tinylsm
