#pragma once

// Builds SST data/index block_contents (format.md §5.2 / §5.3).
// Full internal keys, no prefix compression; restart array at end.

#include "block_format.h"

#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = kBlockRestartInterval);

  // Discard buffered entries and restarts; ready for a new block.
  void Reset();

  // Append one entry. Keys must be added in comparator order (caller enforces).
  // key/value are raw bytes (for data blocks: full internal key + user value).
  void Add(std::string_view key, std::string_view value);

  // Finalize block_contents: entries || restart_offsets... || num_restarts.
  // Does NOT append the 5-byte on-disk trailer (use AppendBlockTrailer).
  // The returned view is valid until the next mutating call.
  std::string_view Finish();

  // Estimate of encoded size if Finish() were called now (entries + restarts).
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

  // Number of keys added since last Reset / construction.
  int NumEntries() const { return num_entries_; }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;       // entries since last restart
  int num_entries_ = 0;
  bool finished_ = false;
};

}  // namespace tinylsm
