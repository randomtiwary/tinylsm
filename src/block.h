#pragma once

// Immutable decoded SST block_contents + iterator (format.md §5.2).
// Works for both data blocks and index blocks (same entry encoding).

#include "block_format.h"
#include "internal_key.h"
#include "tinylsm/status.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace tinylsm {

// Owns a copy of block_contents (without the 5-byte trailer).
class Block {
 public:
  // Construct from verified block_contents (no trailer).
  // Invalid/corrupt layout is reported via Iterator::status() on use,
  // and via ok() after construction for structural checks we can do eagerly.
  explicit Block(std::string contents);

  // Construct by verifying a full on-disk block (contents + trailer).
  // On CRC/type failure, ok() is false and status() holds Corruption.
  static Block FromTrailerBuffer(std::string_view full_block);

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  std::string_view contents() const {
    return std::string_view(contents_.data(), contents_.size());
  }

  uint32_t NumRestarts() const { return num_restarts_; }

  // Bidirectional iterator over entries. Uses InternalKeyComparator for Seek.
  class Iterator {
   public:
    Iterator(const Block* block, const InternalKeyComparator* cmp);

    bool Valid() const { return valid_; }
    const Status& status() const { return status_; }

    std::string_view key() const {
      assert(Valid());
      return key_;
    }
    std::string_view value() const {
      assert(Valid());
      return value_;
    }

    void SeekToFirst();
    void SeekToLast();
    // Position at first entry with key >= target (internal-key order).
    void Seek(std::string_view target);
    void Next();
    void Prev();

   private:
    void Corruption(const char* msg);
    uint32_t NumRestarts() const { return block_->num_restarts_; }
    uint32_t RestartOffset(uint32_t index) const;
    void SeekToRestartPoint(uint32_t index);
    // Parse next entry starting at current_; sets key_/value_/valid_.
    bool ParseNextKey();

    const Block* block_;
    const InternalKeyComparator* cmp_;
    Status status_;
    const char* current_ = nullptr;  // start of current entry (or limit if invalid)
    const char* limit_ = nullptr;    // start of restart array
    std::string_view key_;
    std::string_view value_;
    uint32_t restart_index_ = 0;
    bool valid_ = false;
  };

  Iterator NewIterator(const InternalKeyComparator* cmp) const {
    return Iterator(this, cmp);
  }

 private:
  // Parse restart footer from contents_; set status_ on failure.
  void Initialize();

  std::string contents_;
  Status status_;
  uint32_t num_restarts_ = 0;
  // Byte offset of restart[0] within contents_ (also end of entries region).
  uint32_t restart_offset_ = 0;
};

}  // namespace tinylsm
