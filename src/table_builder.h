#pragma once

// Builds an SSTable on a WritableFile (format.md §5).
// Keys must be added in InternalKeyComparator order.

#include "block_builder.h"
#include "table_format.h"
#include "tinylsm/env.h"
#include "tinylsm/status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

class TableBuilder {
 public:
  // Writes SST bytes to *file (not owned). block_size is the target for data
  // block_contents before flush (default 4 KiB).
  // bloom_bits_per_key > 0 enables a whole-table bloom over user keys
  // (filter block + non-zero filter_handle); 0 leaves filter off (handle 0,0).
  explicit TableBuilder(WritableFile* file,
                        size_t block_size = kDefaultBlockSize,
                        int bloom_bits_per_key = 0);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Append one entry. Keys must be strictly increasing in internal-key order
  // (caller enforces). value may be empty (e.g. deletion tombstone payload).
  void Add(std::string_view internal_key, std::string_view value);

  // Flush remaining data block (or one empty data block if no keys), write
  // optional filter block, index block + footer. *stats receives file size
  // and key range. After Finish, further Add/Finish is invalid.
  Status Finish(TableBuildStats* stats);

  // Discard without writing a complete table. Further Add/Finish invalid.
  void Abandon();

  // Sticky status from the first failed Append (or OK).
  Status status() const { return status_; }

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }

 private:
  // Finish current data block, append with trailer, record index entry.
  // For a never-touched empty block when empty_index_entry is true, still
  // writes the empty data block but does not add an index entry (empty table).
  void FlushDataBlock(bool add_index_entry);

  void WriteRawBlock(std::string_view contents, BlockHandle* handle);

  WritableFile* file_;
  const size_t block_size_;
  const int bloom_bits_per_key_;
  Status status_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  uint64_t num_data_blocks_ = 0;
  bool closed_ = false;

  BlockBuilder data_block_;
  BlockBuilder index_block_;

  std::string last_key_;
  std::string smallest_key_;
  bool has_smallest_ = false;
  // True if the current data_block_ has received at least one key, or we need
  // an empty data block at Finish for a zero-key table.
  bool pending_empty_data_block_ = true;

  // Owned user-key copies for bloom when enabled (avoids dangling views).
  std::vector<std::string> bloom_user_keys_;
};

}  // namespace tinylsm
