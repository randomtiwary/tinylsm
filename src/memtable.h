#pragma once

// In-memory write buffer backed by a skiplist of internal-key ordered entries.
//
// Concurrency model (LevelDB-style educational subset):
//   - MemTable is externally synchronized: callers hold a DB mutex for Add and
//     for any mutation of shared state.
//   - No internal locks.
//   - Concurrent readers are only safe when no writer is active.

#include "internal_key.h"
#include "skiplist.h"

#include "tinylsm/status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

class MemTable {
 public:
  // Comparator for length-prefixed entries stored in the skiplist.
  // Each key is: varint32(internal_key_len) || internal_key ||
  //              varint32(value_len) || value
  struct KeyComparator {
    const InternalKeyComparator comparator;

    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}

    int Compare(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  explicit MemTable(const InternalKeyComparator& comparator);
  ~MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Add an entry for "key" at "sequence" with "type" and "value".
  // For deletions, pass an empty "value" (ignored by Get semantics).
  // REQUIRES: external synchronization (single writer / DB mutex held).
  void Add(SequenceNumber sequence, ValueType type, std::string_view key,
           std::string_view value);

  // If memtable contains a value for key.user_key() visible at the lookup
  // sequence, stores it in *value and returns OK.
  // If the newest visible entry is a deletion, or no entry exists: NotFound.
  Status Get(const LookupKey& key, std::string* value) const;

  // Approximate memory used by this memtable (entries + skiplist overhead).
  size_t ApproximateMemoryUsage() const { return approx_memory_usage_; }

  // Iterator over entries in internal-key order.
  // key() is the internal key (user_key || trailer); value() is the value.
  class Iterator {
   public:
    explicit Iterator(const MemTable* table);

    bool Valid() const;
    void SeekToFirst();
    // Position at first entry whose internal key is >= target.
    void Seek(std::string_view internal_key);
    void Next();

    // REQUIRES: Valid()
    std::string_view key() const;
    // REQUIRES: Valid()
    std::string_view value() const;

   private:
    const MemTable* table_;
    Table::Iterator iter_;
    mutable std::string seek_scratch_;
  };

  // Iterator valid for the lifetime of this MemTable (no concurrent Add).
  Iterator NewIterator() const { return Iterator(this); }

 private:
  static std::string_view GetLengthPrefixedInternalKey(const char* entry);
  static std::string_view GetLengthPrefixedValue(const char* entry);

  KeyComparator comparator_;
  Table table_;
  // Own entry buffers allocated with new char[].
  std::vector<char*> arena_;
  size_t approx_memory_usage_ = 0;
};

}  // namespace tinylsm
