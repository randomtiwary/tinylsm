#include "block_builder.h"

#include "tinylsm/coding.h"

#include <cassert>

namespace tinylsm {

BlockBuilder::BlockBuilder(int restart_interval)
    : restart_interval_(restart_interval) {
  assert(restart_interval_ >= 1);
  restarts_.push_back(0);  // first restart at offset 0 even for empty blocks
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  num_entries_ = 0;
  finished_ = false;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(counter_ <= restart_interval_);

  // New restart point every restart_interval_ keys (first is already 0).
  if (counter_ >= restart_interval_) {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  // v1: full key, no shared-prefix compression (format.md §5.2).
  PutVarint32(&buffer_, static_cast<uint32_t>(key.size()));
  buffer_.append(key.data(), key.size());
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(value.data(), value.size());

  ++counter_;
  ++num_entries_;
}

std::string_view BlockBuilder::Finish() {
  // Append restart array + count (format.md §5.2).
  for (uint32_t off : restarts_) {
    PutFixed32(&buffer_, off);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return std::string_view(buffer_.data(), buffer_.size());
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  // entries + 4 bytes per restart + 4 bytes for num_restarts
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) +
         sizeof(uint32_t);
}

}  // namespace tinylsm
