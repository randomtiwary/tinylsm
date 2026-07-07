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

Status Table::Get(std::string_view internal_key, std::string* value) const {
  assert(value != nullptr);
  if (internal_key.size() < 8) {
    return Status::Corruption("Get: internal key too short");
  }

  // 1) Find data block via index: first index key >= target (last key of block).
  auto index_it = index_block_.NewIterator(&icmp_);
  index_it.Seek(internal_key);
  if (!index_it.status().ok()) {
    return index_it.status();
  }
  if (!index_it.Valid()) {
    return Status::NotFound("key not found");
  }

  BlockHandle data_handle;
  std::string_view handle_enc = index_it.value();
  Status s = data_handle.DecodeFrom(&handle_enc);
  if (!s.ok()) {
    return s;
  }

  // 2) Load data block (CRC verified) and seek.
  Block data_block{std::string{}};
  s = ReadBlock(data_handle, &data_block);
  if (!s.ok()) {
    return s;
  }

  auto data_it = data_block.NewIterator(&icmp_);
  data_it.Seek(internal_key);
  if (!data_it.status().ok()) {
    return data_it.status();
  }
  if (!data_it.Valid()) {
    return Status::NotFound("key not found");
  }

  // 3) Same user key? Apply value / deletion (format.md §3.3 LookupKey semantics).
  const std::string_view found_key = data_it.key();
  if (found_key.size() < 8) {
    return Status::Corruption("SST entry key too short");
  }
  const std::string_view want_user = ExtractUserKey(internal_key);
  const std::string_view found_user = ExtractUserKey(found_key);
  if (want_user != found_user) {
    return Status::NotFound("key not found");
  }

  const ValueType type = ExtractValueType(found_key);
  if (type == kTypeDeletion) {
    return Status::NotFound("deleted");
  }
  if (type != kTypeValue) {
    return Status::Corruption("unknown value type in SST");
  }

  value->assign(data_it.value().data(), data_it.value().size());
  return Status::OK();
}

Status Table::Get(std::string_view user_key, SequenceNumber snapshot,
                  std::string* value) const {
  LookupKey lkey(user_key, snapshot);
  return Get(lkey.internal_key(), value);
}

}  // namespace tinylsm
