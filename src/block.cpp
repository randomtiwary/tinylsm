#include "block.h"

#include "tinylsm/coding.h"

#include <cassert>

namespace tinylsm {

Block::Block(std::string contents) : contents_(std::move(contents)) {
  Initialize();
}

Block Block::FromTrailerBuffer(std::string_view full_block) {
  std::string_view contents_view;
  Status st = VerifyBlockTrailer(full_block, &contents_view);
  if (!st.ok()) {
    Block bad{std::string{}};
    // Replace empty-block init status with the trailer verification error.
    bad.contents_.clear();
    bad.status_ = st;
    bad.num_restarts_ = 0;
    bad.restart_offset_ = 0;
    return bad;
  }
  return Block(std::string(contents_view));
}

void Block::Initialize() {
  const size_t n = contents_.size();
  // Need at least num_restarts (4 bytes). A well-formed empty block is
  // restart[0]=0 + num_restarts=1 → 8 bytes.
  if (n < sizeof(uint32_t)) {
    status_ = Status::Corruption("block contents too short");
    num_restarts_ = 0;
    restart_offset_ = 0;
    return;
  }
  num_restarts_ = DecodeFixed32(contents_.data() + n - sizeof(uint32_t));
  if (num_restarts_ == 0) {
    status_ = Status::Corruption("block has zero restarts");
    restart_offset_ = 0;
    return;
  }
  const size_t restart_array_bytes =
      static_cast<size_t>(num_restarts_) * sizeof(uint32_t);
  if (restart_array_bytes + sizeof(uint32_t) > n) {
    status_ = Status::Corruption("block restart array out of bounds");
    num_restarts_ = 0;
    restart_offset_ = 0;
    return;
  }
  restart_offset_ =
      static_cast<uint32_t>(n - sizeof(uint32_t) - restart_array_bytes);
  status_ = Status::OK();
}

// ---------------------------------------------------------------------------
// Iterator
// ---------------------------------------------------------------------------

Block::Iterator::Iterator(const Block* block, const InternalKeyComparator* cmp)
    : block_(block), cmp_(cmp), status_(block->status()) {
  assert(cmp_ != nullptr);
  if (!status_.ok()) {
    valid_ = false;
    current_ = nullptr;
    limit_ = nullptr;
    return;
  }
  limit_ = block_->contents_.data() + block_->restart_offset_;
  current_ = limit_;
  valid_ = false;
}

void Block::Iterator::Corruption(const char* msg) {
  status_ = Status::Corruption(msg);
  valid_ = false;
  current_ = limit_;
}

uint32_t Block::Iterator::RestartOffset(uint32_t index) const {
  assert(index < NumRestarts());
  const char* p = block_->contents_.data() + block_->restart_offset_ +
                  index * sizeof(uint32_t);
  return DecodeFixed32(p);
}

void Block::Iterator::SeekToRestartPoint(uint32_t index) {
  key_ = std::string_view();
  value_ = std::string_view();
  restart_index_ = index;
  const uint32_t offset = RestartOffset(index);
  if (offset > block_->restart_offset_) {
    Corruption("restart offset past entries");
    return;
  }
  current_ = block_->contents_.data() + offset;
}

bool Block::Iterator::ParseNextKey() {
  if (!status_.ok()) {
    valid_ = false;
    return false;
  }
  if (current_ >= limit_) {
    current_ = limit_;
    valid_ = false;
    return false;
  }

  // Decode: varint key_len | key | varint value_len | value  (format.md §5.2)
  uint32_t key_len = 0;
  const char* p = GetVarint32Ptr(current_, limit_, &key_len);
  if (p == nullptr) {
    Corruption("bad key length");
    return false;
  }
  if (static_cast<size_t>(limit_ - p) < key_len) {
    Corruption("key extends past entries");
    return false;
  }
  key_ = std::string_view(p, key_len);
  p += key_len;

  uint32_t value_len = 0;
  p = GetVarint32Ptr(p, limit_, &value_len);
  if (p == nullptr) {
    Corruption("bad value length");
    return false;
  }
  if (static_cast<size_t>(limit_ - p) < value_len) {
    Corruption("value extends past entries");
    return false;
  }
  value_ = std::string_view(p, value_len);
  p += value_len;

  current_ = p;
  valid_ = true;

  // Advance restart_index_ so it names a restart at or before this entry.
  // Used by Prev() to know which region to re-scan.
  while (restart_index_ + 1 < NumRestarts() &&
         block_->contents_.data() + RestartOffset(restart_index_ + 1) <
             current_) {
    ++restart_index_;
  }
  return true;
}

void Block::Iterator::SeekToFirst() {
  if (!status_.ok() || NumRestarts() == 0) {
    valid_ = false;
    return;
  }
  SeekToRestartPoint(0);
  ParseNextKey();
}

void Block::Iterator::SeekToLast() {
  if (!status_.ok() || NumRestarts() == 0) {
    valid_ = false;
    return;
  }
  SeekToRestartPoint(NumRestarts() - 1);
  // Walk to the last entry in the final restart region.
  bool have = false;
  std::string_view last_key;
  std::string_view last_value;
  const char* last_current = limit_;
  while (ParseNextKey()) {
    have = true;
    last_key = key_;
    last_value = value_;
    last_current = current_;
  }
  if (!have) {
    // Empty block: restarts present but no entries (limit_ at offset 0).
    valid_ = false;
    return;
  }
  key_ = last_key;
  value_ = last_value;
  current_ = last_current;
  valid_ = true;
  // ParseNextKey left valid_=false after the failed parse past the end;
  // restore valid state for the last entry.
  status_ = block_->status();  // clear any confusion; should still be OK
}

void Block::Iterator::Seek(std::string_view target) {
  if (!status_.ok() || NumRestarts() == 0) {
    valid_ = false;
    return;
  }

  // Binary search for the last restart whose first key is < target.
  uint32_t left = 0;
  uint32_t right = NumRestarts() - 1;
  while (left < right) {
    const uint32_t mid = (left + right + 1) / 2;
    const uint32_t region_offset = RestartOffset(mid);
    if (region_offset > block_->restart_offset_) {
      Corruption("restart offset past entries");
      return;
    }
    const char* region = block_->contents_.data() + region_offset;
    uint32_t key_len = 0;
    const char* p = GetVarint32Ptr(region, limit_, &key_len);
    if (p == nullptr || static_cast<size_t>(limit_ - p) < key_len) {
      Corruption("bad key at restart");
      return;
    }
    const std::string_view mid_key(p, key_len);
    if (cmp_->Compare(mid_key, target) < 0) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }

  // Linear scan for the first key >= target.
  SeekToRestartPoint(left);
  while (true) {
    if (!ParseNextKey()) {
      return;
    }
    if (cmp_->Compare(key_, target) >= 0) {
      return;
    }
  }
}

void Block::Iterator::Next() {
  assert(Valid());
  ParseNextKey();
}

void Block::Iterator::Prev() {
  assert(Valid());

  // current_ points just past the current entry. Find the previous entry by
  // re-scanning from an appropriate restart point.
  const char* const end_of_current = current_;

  // If the current entry starts at or before restart[restart_index_], step
  // back one restart so we have room to land on a previous entry.
  // The start of the current entry is unknown; end_of_current is known.
  // Use restart_index_ as maintained by ParseNextKey (region containing current).
  while (true) {
    const char* region_start =
        block_->contents_.data() + RestartOffset(restart_index_);
    // Scan this region for the entry that ends at end_of_current; the one
    // before it is the predecessor.
    current_ = region_start;
    key_ = std::string_view();
    value_ = std::string_view();

    std::string_view prev_key;
    std::string_view prev_value;
    const char* prev_end = nullptr;
    bool have_prev = false;

    while (current_ < end_of_current && current_ < limit_) {
      const char* entry_start = current_;
      if (!ParseNextKey()) {
        // Corruption or unexpected end.
        return;
      }
      if (current_ == end_of_current) {
        // key_/value_ are the entry we called Prev() from. Restore previous.
        if (!have_prev) {
          // No previous entry in this restart region — try earlier region.
          break;
        }
        key_ = prev_key;
        value_ = prev_value;
        current_ = prev_end;
        valid_ = true;
        return;
      }
      // Still before the target entry; remember this one as candidate prev.
      prev_key = key_;
      prev_value = value_;
      prev_end = current_;
      have_prev = true;
      (void)entry_start;
    }

    if (restart_index_ == 0) {
      // Already at first region and no predecessor — invalid (before first).
      valid_ = false;
      current_ = limit_;
      return;
    }
    --restart_index_;
  }
}

}  // namespace tinylsm
