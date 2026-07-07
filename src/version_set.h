#pragma once

// Version / VersionSet: immutable COW levels + MANIFEST/CURRENT recovery.
// Educational v1: L0 and L1 only. See design doc §5 and format.md §7–§9.
// No nested mutex — callers hold DBImpl::mutex_ (when DB exists).

#include "version_edit.h"

#include "tinylsm/env.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tinylsm {

// Immutable snapshot of live SST files at L0 and L1.
// Lifetime via shared_ptr; FileMetaData shared across versions that list them.
class Version {
 public:
  static constexpr int kNumLevels = 2;

  Version() = default;

  // levels: 0 = L0, 1 = L1
  const std::vector<std::shared_ptr<FileMetaData>>& LevelFiles(int level) const {
    return levels_[level];
  }

  size_t NumFiles(int level) const { return levels_[level].size(); }

  // Find FileMetaData by number at a level; nullptr if absent.
  std::shared_ptr<FileMetaData> FindFile(int level, uint64_t number) const;

 private:
  friend class VersionSet;
  std::vector<std::shared_ptr<FileMetaData>> levels_[kNumLevels];
};

// Owns the live Version, file-number allocator, and MANIFEST writer state.
class VersionSet {
 public:
  VersionSet(const std::string& dbname, Env* env);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // File-number allocator (design §2.4): allocate vs peek only.
  uint64_t NewFileNumber() { return next_file_number_++; }
  uint64_t PeekNextFileNumber() const { return next_file_number_; }

  // May raise the live counter after recovery; never lower it.
  void MarkFileNumberUsed(uint64_t number);

  void SetLastSequence(uint64_t s) { last_sequence_ = s; }
  uint64_t LastSequence() const { return last_sequence_; }

  uint64_t LogNumber() const { return log_number_; }
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  std::shared_ptr<Version> current() const { return current_; }

  // Create a brand-new DB directory state (format.md §9):
  // empty Version, MANIFEST-1 with seed edit, CURRENT, empty 1.log.
  Status NewDB();

  // Recover from CURRENT → MANIFEST: apply all edits from empty base.
  Status Recover();

  // COW apply: build new Version from current + edit, append framed edit to
  // MANIFEST (synced), update CURRENT if a new manifest was created, publish.
  Status LogAndApply(VersionEdit* edit);

 private:
  Status ApplyEdit(const VersionEdit& edit, Version* v);
  Status WriteEditToManifest(const VersionEdit& edit);
  Status SetCurrentFile(uint64_t manifest_number);
  Status ReadCurrentFile(std::string* manifest_basename);
  Status OpenManifestForAppend(uint64_t manifest_number);
  Status ReplayManifest(SequentialFile* file, Version* builder);

  // Deep-copy file lists into a new Version (COW base for LogAndApply).
  static std::shared_ptr<Version> CloneVersion(const Version& src);

  std::string dbname_;
  Env* env_;

  std::shared_ptr<Version> current_;
  uint64_t next_file_number_ = 2;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;
  uint64_t manifest_file_number_ = 0;

  std::unique_ptr<WritableFile> descriptor_log_;
};

}  // namespace tinylsm
