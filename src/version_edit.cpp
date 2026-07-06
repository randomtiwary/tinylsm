#include "version_edit.h"

#include "tinylsm/coding.h"

namespace tinylsm {

void VersionEdit::Clear() {
  has_comparator_ = false;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  comparator_.clear();
  log_number_ = 0;
  next_file_number_ = 0;
  last_sequence_ = 0;
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::SetComparatorName(std::string_view name) {
  has_comparator_ = true;
  comparator_.assign(name.data(), name.size());
}

void VersionEdit::SetLogNumber(uint64_t num) {
  has_log_number_ = true;
  log_number_ = num;
}

void VersionEdit::SetNextFileNumber(uint64_t num) {
  has_next_file_number_ = true;
  next_file_number_ = num;
}

void VersionEdit::SetLastSequence(uint64_t seq) {
  has_last_sequence_ = true;
  last_sequence_ = seq;
}

void VersionEdit::AddFile(int level, uint64_t file, uint64_t file_size,
                          std::string_view smallest,
                          std::string_view largest) {
  auto meta = std::make_shared<FileMetaData>();
  meta->number = file;
  meta->file_size = file_size;
  meta->smallest.assign(smallest.data(), smallest.size());
  meta->largest.assign(largest.data(), largest.size());
  new_files_.emplace_back(level, std::move(meta));
}

void VersionEdit::DeleteFile(int level, uint64_t file) {
  deleted_files_.insert({level, file});
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& del : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(del.first));
    PutVarint64(dst, del.second);
  }
  for (const auto& nf : new_files_) {
    const FileMetaData& f = *nf.second;
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(nf.first));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest);
    PutLengthPrefixedSlice(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;
  while (!input.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) {
      return Status::Corruption("VersionEdit: bad tag varint");
    }
    switch (tag) {
      case kComparator: {
        std::string_view name;
        if (!GetLengthPrefixedSlice(&input, &name)) {
          return Status::Corruption("VersionEdit: bad comparator");
        }
        SetComparatorName(name);
        break;
      }
      case kLogNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: bad log number");
        }
        SetLogNumber(v);
        break;
      }
      case kNextFileNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: bad next file number");
        }
        SetNextFileNumber(v);
        break;
      }
      case kLastSequence: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: bad last sequence");
        }
        SetLastSequence(v);
        break;
      }
      case kCompactPointer: {
        // Unused in v1: level (varint32) + length-prefixed internal key.
        uint32_t level = 0;
        std::string_view key;
        if (!GetVarint32(&input, &level) ||
            !GetLengthPrefixedSlice(&input, &key)) {
          return Status::Corruption("VersionEdit: bad compact pointer");
        }
        (void)level;
        (void)key;
        break;
      }
      case kDeletedFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number)) {
          return Status::Corruption("VersionEdit: bad deleted file");
        }
        DeleteFile(static_cast<int>(level), number);
        break;
      }
      case kNewFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        uint64_t file_size = 0;
        std::string_view smallest;
        std::string_view largest;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number) ||
            !GetVarint64(&input, &file_size) ||
            !GetLengthPrefixedSlice(&input, &smallest) ||
            !GetLengthPrefixedSlice(&input, &largest)) {
          return Status::Corruption("VersionEdit: bad new file");
        }
        AddFile(static_cast<int>(level), number, file_size, smallest, largest);
        break;
      }
      case kPrevLogNumber: {
        // Unused in v1: ignore varint64 body.
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: bad prev log number");
        }
        (void)v;
        break;
      }
      default:
        return Status::Corruption("VersionEdit: unknown tag " +
                                  std::to_string(tag));
    }
  }
  return Status::OK();
}

}  // namespace tinylsm
