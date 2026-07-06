#pragma once

// Immutable SSTable reader: Open via RandomAccessFile, Get by internal key
// / LookupKey (format.md §5). Verifies block trailers (CRC) on load.

#include "block.h"
#include "internal_key.h"
#include "table_format.h"
#include "tinylsm/env.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tinylsm {

class Table {
 public:
  // Open an existing SST. *file is not owned and must outlive the Table.
  // file_size must be the full size of the file (needed to locate the footer).
  static Status Open(RandomAccessFile* file, uint64_t file_size,
                     std::unique_ptr<Table>* table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Layered Get (same contract as MemTable::Get):
  //   true  — this SST has a definitive entry for the user key;
  //           *s is OK (value) or NotFound (deletion tombstone — stop older files).
  //   false — user key not present in this file (*s is NotFound, or error in *s).
  // On Corruption/IO error: returns true with *s set so callers stop the search.
  bool Get(std::string_view internal_key, std::string* value, Status* s) const;

  // Convenience wrappers that collapse to a single Status (tests / simple callers).
  // Deletion and missing both appear as NotFound (use bool form for layered Get).
  Status Get(std::string_view internal_key, std::string* value) const;

  // Convenience: build LookupKey(user_key, snapshot) and call Status Get.
  Status Get(std::string_view user_key, SequenceNumber snapshot,
             std::string* value) const;

  const Footer& footer() const { return footer_; }
  uint64_t file_size() const { return file_size_; }

 private:
  Table(RandomAccessFile* file, uint64_t file_size, Footer footer,
        Block index_block);

  // Read handle.size contents + 5-byte trailer; verify CRC; return Block.
  Status ReadBlock(const BlockHandle& handle, Block* out) const;

  RandomAccessFile* file_;
  uint64_t file_size_;
  Footer footer_;
  Block index_block_;
  InternalKeyComparator icmp_;
};

}  // namespace tinylsm
