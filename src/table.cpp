#include "table.h"

#include "block_format.h"
#include "tinylsm/coding.h"

#include <cassert>
#include <utility>

namespace tinylsm {

Table::Table(RandomAccessFile* file, uint64_t file_size, Footer footer,
             Block index_block)
    : file_(file),
      file_size_(file_size),
      footer_(footer),
      index_block_(std::move(index_block)) {}

Status Table::Open(RandomAccessFile* file, uint64_t file_size,
                   std::unique_ptr<Table>* table) {
  assert(file != nullptr);
  assert(table != nullptr);
  table->reset();

  if (file_size < kFooterSize) {
    return Status::Corruption("file too short for SST footer");
  }

  // Footer is the last 48 bytes.
  std::string footer_buf;
  Status s =
      file->Read(file_size - kFooterSize, kFooterSize, &footer_buf);
  if (!s.ok()) {
    return s;
  }
  if (footer_buf.size() != kFooterSize) {
    return Status::Corruption("short read of SST footer");
  }

  Footer footer;
  s = footer.DecodeFrom(footer_buf);
  if (!s.ok()) {
    return s;
  }

  // Load index block (contents size excludes trailer).
  const BlockHandle& ih = footer.index_handle;
  if (ih.size > file_size || ih.offset > file_size ||
      ih.offset + ih.size + kBlockTrailerSize > file_size) {
    return Status::Corruption("index handle out of file bounds");
  }

  const size_t index_total =
      static_cast<size_t>(ih.size) + kBlockTrailerSize;
  std::string index_raw;
  s = file->Read(ih.offset, index_total, &index_raw);
  if (!s.ok()) {
    return s;
  }
  if (index_raw.size() != index_total) {
    return Status::Corruption("short read of index block");
  }

  Block index_block = Block::FromTrailerBuffer(index_raw);
  if (!index_block.ok()) {
    return index_block.status();
  }

  table->reset(new Table(file, file_size, footer, std::move(index_block)));
  return Status::OK();
}

Status Table::ReadBlock(const BlockHandle& handle, Block* out) const {
  assert(out != nullptr);
  if (handle.size > file_size_ || handle.offset > file_size_ ||
      handle.offset + handle.size + kBlockTrailerSize > file_size_) {
    return Status::Corruption("block handle out of file bounds");
  }
  const size_t total = static_cast<size_t>(handle.size) + kBlockTrailerSize;
  std::string raw;
  Status s = file_->Read(handle.offset, total, &raw);
  if (!s.ok()) {
    return s;
  }
  if (raw.size() != total) {
    return Status::Corruption("short read of data block");
  }
  *out = Block::FromTrailerBuffer(raw);
  if (!out->ok()) {
    return out->status();
  }
  return Status::OK();
}

bool Table::Get(std::string_view internal_key, std::string* value,
                Status* s) const {
  assert(value != nullptr);
  assert(s != nullptr);
  if (internal_key.size() < 8) {
    *s = Status::Corruption("Get: internal key too short");
    return true;  // hard error: stop layered search
  }

  // 1) Find data block via index: first index key >= target (last key of block).
  auto index_it = index_block_.NewIterator(&icmp_);
  index_it.Seek(internal_key);
  if (!index_it.status().ok()) {
    *s = index_it.status();
    return true;
  }
  if (!index_it.Valid()) {
    *s = Status::NotFound("key not found");
    return false;
  }

  BlockHandle data_handle;
  std::string_view handle_enc = index_it.value();
  Status st = data_handle.DecodeFrom(&handle_enc);
  if (!st.ok()) {
    *s = st;
    return true;
  }

  // 2) Load data block (CRC verified) and seek.
  Block data_block{std::string{}};
  st = ReadBlock(data_handle, &data_block);
  if (!st.ok()) {
    *s = st;
    return true;
  }

  auto data_it = data_block.NewIterator(&icmp_);
  data_it.Seek(internal_key);
  if (!data_it.status().ok()) {
    *s = data_it.status();
    return true;
  }
  if (!data_it.Valid()) {
    *s = Status::NotFound("key not found");
    return false;
  }

  // 3) Same user key? Apply value / deletion (format.md §3.3 LookupKey semantics).
  const std::string_view found_key = data_it.key();
  if (found_key.size() < 8) {
    *s = Status::Corruption("SST entry key too short");
    return true;
  }
  const std::string_view want_user = ExtractUserKey(internal_key);
  const std::string_view found_user = ExtractUserKey(found_key);
  if (want_user != found_user) {
    *s = Status::NotFound("key not found");
    return false;
  }

  const ValueType type = ExtractValueType(found_key);
  if (type == kTypeDeletion) {
    // Tombstone in this SST: stop older files (L0 reverse / L1) from resurrecting.
    *s = Status::NotFound("deleted");
    return true;
  }
  if (type != kTypeValue) {
    *s = Status::Corruption("unknown value type in SST");
    return true;
  }

  value->assign(data_it.value().data(), data_it.value().size());
  *s = Status::OK();
  return true;
}

Status Table::Get(std::string_view internal_key, std::string* value) const {
  Status s;
  if (Get(internal_key, value, &s)) {
    return s;
  }
  return Status::NotFound("key not found");
}

Status Table::Get(std::string_view user_key, SequenceNumber snapshot,
                  std::string* value) const {
  LookupKey lkey(user_key, snapshot);
  return Get(lkey.internal_key(), value);
}

// ---------------------------------------------------------------------------
// Full-table iterator (index walk + data blocks)
// ---------------------------------------------------------------------------

Table::Iterator::Iterator(const Table* table)
    : table_(table),
      index_iter_(table->index_block_.NewIterator(&table->icmp_)) {
  assert(table_ != nullptr);
}

bool Table::Iterator::Valid() const {
  return status_.ok() && data_iter_ != nullptr && data_iter_->Valid();
}

std::string_view Table::Iterator::key() const {
  assert(Valid());
  return data_iter_->key();
}

std::string_view Table::Iterator::value() const {
  assert(Valid());
  return data_iter_->value();
}

void Table::Iterator::InitDataBlock() {
  data_iter_.reset();
  data_block_.reset();
  if (!status_.ok() || !index_iter_.Valid()) {
    return;
  }
  if (!index_iter_.status().ok()) {
    status_ = index_iter_.status();
    return;
  }

  BlockHandle handle;
  std::string_view enc = index_iter_.value();
  Status st = handle.DecodeFrom(&enc);
  if (!st.ok()) {
    status_ = st;
    return;
  }

  auto block = std::make_unique<Block>(std::string{});
  st = table_->ReadBlock(handle, block.get());
  if (!st.ok()) {
    status_ = st;
    return;
  }
  data_block_ = std::move(block);
  data_iter_ = std::make_unique<Block::Iterator>(
      data_block_->NewIterator(&table_->icmp_));
  data_iter_->SeekToFirst();
  if (!data_iter_->status().ok()) {
    status_ = data_iter_->status();
    data_iter_.reset();
    data_block_.reset();
  }
}

void Table::Iterator::SeekToFirst() {
  status_ = Status::OK();
  data_iter_.reset();
  data_block_.reset();
  index_iter_ = table_->index_block_.NewIterator(&table_->icmp_);
  index_iter_.SeekToFirst();
  if (!index_iter_.status().ok()) {
    status_ = index_iter_.status();
    return;
  }
  // Skip empty data blocks (should be rare).
  while (index_iter_.Valid()) {
    InitDataBlock();
    if (!status_.ok()) {
      return;
    }
    if (data_iter_ != nullptr && data_iter_->Valid()) {
      return;
    }
    index_iter_.Next();
  }
}

void Table::Iterator::Next() {
  assert(Valid());
  data_iter_->Next();
  if (!data_iter_->status().ok()) {
    status_ = data_iter_->status();
    data_iter_.reset();
    data_block_.reset();
    return;
  }
  if (data_iter_->Valid()) {
    return;
  }
  // Advance to next data block.
  index_iter_.Next();
  while (index_iter_.Valid()) {
    InitDataBlock();
    if (!status_.ok()) {
      return;
    }
    if (data_iter_ != nullptr && data_iter_->Valid()) {
      return;
    }
    index_iter_.Next();
  }
  data_iter_.reset();
  data_block_.reset();
}

}  // namespace tinylsm
