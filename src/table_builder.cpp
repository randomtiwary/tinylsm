#include "table_builder.h"

#include "block_format.h"

#include <cassert>
#include <string>

namespace tinylsm {

TableBuilder::TableBuilder(WritableFile* file, size_t block_size)
    : file_(file), block_size_(block_size) {
  assert(file_ != nullptr);
  assert(block_size_ >= 1);
}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  assert(!closed_);
  if (!status_.ok()) {
    return;
  }
  assert(internal_key.size() >= 8);

  if (!has_smallest_) {
    smallest_key_.assign(internal_key.data(), internal_key.size());
    has_smallest_ = true;
  }
  last_key_.assign(internal_key.data(), internal_key.size());

  data_block_.Add(internal_key, value);
  pending_empty_data_block_ = false;
  ++num_entries_;

  // Flush after finishing an entry when estimate meets target (format.md §5.5).
  if (data_block_.CurrentSizeEstimate() >= block_size_) {
    FlushDataBlock(/*add_index_entry=*/true);
  }
}

void TableBuilder::WriteRawBlock(std::string_view contents,
                                 BlockHandle* handle) {
  assert(handle != nullptr);
  if (!status_.ok()) {
    return;
  }
  handle->offset = offset_;
  handle->size = contents.size();

  std::string buf;
  buf.reserve(contents.size() + kBlockTrailerSize);
  buf.assign(contents.data(), contents.size());
  AppendBlockTrailer(&buf);

  status_ = file_->Append(buf);
  if (status_.ok()) {
    offset_ += buf.size();
  }
}

void TableBuilder::FlushDataBlock(bool add_index_entry) {
  if (!status_.ok()) {
    return;
  }

  // last_key_ is the separator key for this block when add_index_entry.
  BlockHandle handle;
  std::string_view contents = data_block_.Finish();
  WriteRawBlock(contents, &handle);
  data_block_.Reset();
  ++num_data_blocks_;
  pending_empty_data_block_ = false;

  if (add_index_entry && status_.ok()) {
    // Index key = last internal key of the data block (format.md §5.3).
    index_block_.Add(last_key_, handle.Encode());
  }
}

Status TableBuilder::Finish(TableBuildStats* stats) {
  assert(!closed_);
  closed_ = true;
  if (!status_.ok()) {
    return status_;
  }

  // Flush open data block, or write one empty data block for a zero-key table
  // (format.md §5: empty table = one empty data block + empty index).
  if (!data_block_.empty() || pending_empty_data_block_) {
    // Empty table: write empty data block but do not index it.
    const bool add_index = !data_block_.empty();
    // If we have entries, last_key_ is set. Empty block: no index entry.
    FlushDataBlock(/*add_index_entry=*/add_index);
  }

  if (!status_.ok()) {
    return status_;
  }

  // Index block (may be empty: zero entries, still valid restarts).
  BlockHandle index_handle;
  {
    std::string_view contents = index_block_.Finish();
    WriteRawBlock(contents, &index_handle);
  }
  if (!status_.ok()) {
    return status_;
  }

  // Footer: index handle, zero filter handle, padding 0, magic TINYLSM1.
  Footer footer;
  footer.index_handle = index_handle;
  // filter_handle left at (0,0)
  std::string footer_bytes = footer.Encode();
  assert(footer_bytes.size() == kFooterSize);
  status_ = file_->Append(footer_bytes);
  if (status_.ok()) {
    offset_ += footer_bytes.size();
  }
  if (!status_.ok()) {
    return status_;
  }

  if (stats != nullptr) {
    stats->file_size = offset_;
    stats->smallest_key = smallest_key_;
    stats->largest_key = last_key_;
    stats->num_entries = num_entries_;
    stats->num_data_blocks = num_data_blocks_;
  }
  return status_;
}

void TableBuilder::Abandon() {
  closed_ = true;
}

}  // namespace tinylsm
