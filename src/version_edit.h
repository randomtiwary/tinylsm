#pragma once

// VersionEdit: tagged-field logical payload for MANIFEST records.
// See docs/format.md §6 and design doc §5.1.

#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinylsm {

// Live SSTable metadata held by a Version (shared across COW snapshots).
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  // Encoded internal keys (user_key || fixed64_le trailer).
  std::string smallest;
  std::string largest;
};

// Normative comparator name written on NewDB (format.md §6 / §9).
inline constexpr const char kComparatorName[] = "tinylsm.BytewiseComparator";

// A delta applied to Version metadata and persisted as one MANIFEST record.
class VersionEdit {
 public:
  void Clear();

  void SetComparatorName(std::string_view name);
  void SetLogNumber(uint64_t num);
  void SetNextFileNumber(uint64_t num);
  void SetLastSequence(uint64_t seq);

  // level must be 0 or 1 in v1. smallest/largest are full encoded internal keys.
  void AddFile(int level, uint64_t file, uint64_t file_size,
               std::string_view smallest, std::string_view largest);

  void DeleteFile(int level, uint64_t file);

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

  bool HasComparatorName() const { return has_comparator_; }
  bool HasLogNumber() const { return has_log_number_; }
  bool HasNextFileNumber() const { return has_next_file_number_; }
  bool HasLastSequence() const { return has_last_sequence_; }

  const std::string& ComparatorName() const { return comparator_; }
  uint64_t LogNumber() const { return log_number_; }
  uint64_t NextFileNumber() const { return next_file_number_; }
  uint64_t LastSequence() const { return last_sequence_; }

  const std::vector<std::pair<int, std::shared_ptr<FileMetaData>>>& NewFiles()
      const {
    return new_files_;
  }

  // Deleted files as (level, file_number) pairs.
  const std::set<std::pair<int, uint64_t>>& DeletedFiles() const {
    return deleted_files_;
  }

 private:
  // Tag values (format.md §6).
  enum Tag : uint32_t {
    kComparator = 1,
    kLogNumber = 2,
    kNextFileNumber = 3,
    kLastSequence = 4,
    kCompactPointer = 5,  // unused v1; skip body if present
    kDeletedFile = 6,
    kNewFile = 7,
    // 8 reserved / unused
    kPrevLogNumber = 9,  // unused v1; ignore if present
  };

  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  uint64_t last_sequence_ = 0;

  std::set<std::pair<int, uint64_t>> deleted_files_;
  // (level, meta) for new files; shared_ptr so apply can share into Version.
  std::vector<std::pair<int, std::shared_ptr<FileMetaData>>> new_files_;
};

}  // namespace tinylsm
