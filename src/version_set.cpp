#include "version_set.h"

#include "internal_key.h"
#include "tinylsm/coding.h"
#include "tinylsm/crc32c.h"
#include "tinylsm/filename.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace tinylsm {
namespace {

// Basename only: MANIFEST-000001 (matches ManifestFileName padding).
std::string ManifestBaseName(uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return std::string(buf);
}

// Sort L0 by file number ascending; L1 by smallest user key ascending.
void SortLevel(int level, std::vector<std::shared_ptr<FileMetaData>>* files) {
  if (level == 0) {
    std::sort(files->begin(), files->end(),
              [](const std::shared_ptr<FileMetaData>& a,
                 const std::shared_ptr<FileMetaData>& b) {
                return a->number < b->number;
              });
  } else {
    std::sort(files->begin(), files->end(),
              [](const std::shared_ptr<FileMetaData>& a,
                 const std::shared_ptr<FileMetaData>& b) {
                return ExtractUserKey(a->smallest) < ExtractUserKey(b->smallest);
              });
  }
}

// MANIFEST/WAL frame: fixed32_le len | fixed32_le masked_crc | payload.
std::string EncodeManifestRecord(std::string_view payload) {
  std::string record;
  record.reserve(8 + payload.size());
  PutFixed32(&record, static_cast<uint32_t>(payload.size()));
  const uint32_t crc = crc32c::Value(payload.data(), payload.size());
  PutFixed32(&record, crc32c::Mask(crc));
  record.append(payload.data(), payload.size());
  return record;
}

}  // namespace

std::shared_ptr<FileMetaData> Version::FindFile(int level,
                                                uint64_t number) const {
  if (level < 0 || level >= kNumLevels) {
    return nullptr;
  }
  for (const auto& f : levels_[level]) {
    if (f->number == number) {
      return f;
    }
  }
  return nullptr;
}

VersionSet::VersionSet(const std::string& dbname, Env* env)
    : dbname_(dbname), env_(env), current_(std::make_shared<Version>()) {}

VersionSet::~VersionSet() {
  if (descriptor_log_) {
    (void)descriptor_log_->Close();
    descriptor_log_.reset();
  }
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

std::shared_ptr<Version> VersionSet::CloneVersion(const Version& src) {
  auto v = std::make_shared<Version>();
  for (int level = 0; level < Version::kNumLevels; ++level) {
    v->levels_[level] = src.levels_[level];  // shared_ptr copies
  }
  return v;
}

Status VersionSet::SetCurrentFile(uint64_t manifest_number) {
  // format.md §8: CURRENT.tmp with "MANIFEST-{n}\n", fsync, rename, dir fsync.
  const std::string manifest_name = ManifestBaseName(manifest_number);
  const std::string current = CurrentFileName(dbname_);
  const std::string tmp = current + ".tmp";

  {
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(tmp, &file);
    if (!s.ok()) {
      return s;
    }
    s = file->Append(manifest_name);
    if (!s.ok()) {
      return s;
    }
    s = file->Append("\n");
    if (!s.ok()) {
      return s;
    }
    s = file->Sync();
    if (!s.ok()) {
      return s;
    }
    s = file->Close();
    if (!s.ok()) {
      return s;
    }
  }

  Status s = env_->RenameFile(tmp, current);
  if (!s.ok()) {
    (void)env_->DeleteFile(tmp);
    return s;
  }
  (void)env_->FsyncDir(dbname_);  // best-effort
  return Status::OK();
}

Status VersionSet::ReadCurrentFile(std::string* manifest_basename) {
  manifest_basename->clear();
  const std::string current = CurrentFileName(dbname_);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(current, &file);
  if (!s.ok()) {
    return s;
  }
  std::string contents;
  s = file->Read(4096, &contents);
  if (!s.ok()) {
    return s;
  }
  while (!contents.empty() &&
         (contents.back() == '\n' || contents.back() == '\r')) {
    contents.pop_back();
  }
  if (contents.empty()) {
    return Status::Corruption("CURRENT file is empty");
  }
  if (contents.find('\n') != std::string::npos ||
      contents.find('\r') != std::string::npos) {
    return Status::Corruption("CURRENT has multiple lines");
  }
  *manifest_basename = std::move(contents);
  return Status::OK();
}

Status VersionSet::OpenManifestForAppend(uint64_t manifest_number) {
  if (descriptor_log_) {
    (void)descriptor_log_->Close();
    descriptor_log_.reset();
  }
  const std::string path = ManifestFileName(dbname_, manifest_number);
  return env_->NewAppendableFile(path, &descriptor_log_);
}

Status VersionSet::ApplyEdit(const VersionEdit& edit, Version* v) {
  if (edit.HasComparatorName()) {
    if (edit.ComparatorName() != kComparatorName) {
      return Status::InvalidArgument("unknown comparator: " +
                                     edit.ComparatorName());
    }
  }
  if (edit.HasLogNumber()) {
    log_number_ = edit.LogNumber();
  }
  if (edit.HasNextFileNumber()) {
    // Never lower the live counter below what the process has allocated.
    if (edit.NextFileNumber() > next_file_number_) {
      next_file_number_ = edit.NextFileNumber();
    }
  }
  if (edit.HasLastSequence()) {
    last_sequence_ = edit.LastSequence();
  }

  for (const auto& del : edit.DeletedFiles()) {
    const int level = del.first;
    const uint64_t number = del.second;
    if (level < 0 || level >= Version::kNumLevels) {
      return Status::Corruption("VersionEdit delete: bad level");
    }
    auto& files = v->levels_[level];
    files.erase(std::remove_if(files.begin(), files.end(),
                               [number](const std::shared_ptr<FileMetaData>& f) {
                                 return f->number == number;
                               }),
                files.end());
  }

  for (const auto& nf : edit.NewFiles()) {
    const int level = nf.first;
    if (level < 0 || level >= Version::kNumLevels) {
      return Status::Corruption("VersionEdit add: bad level");
    }
    v->levels_[level].push_back(nf.second);
  }

  for (int level = 0; level < Version::kNumLevels; ++level) {
    SortLevel(level, &v->levels_[level]);
  }
  return Status::OK();
}

Status VersionSet::WriteEditToManifest(const VersionEdit& edit) {
  if (!descriptor_log_) {
    return Status::IOError("MANIFEST not open for write");
  }
  std::string payload;
  edit.EncodeTo(&payload);
  const std::string record = EncodeManifestRecord(payload);
  Status s = descriptor_log_->Append(record);
  if (!s.ok()) {
    return s;
  }
  return descriptor_log_->Sync();
}

Status VersionSet::ReplayManifest(SequentialFile* file, Version* builder) {
  // Torn tail → stop successfully; full-frame CRC mismatch → Corruption.
  for (;;) {
    std::string header;
    Status s = file->Read(8, &header);
    if (!s.ok()) {
      return s;
    }
    if (header.size() < 8) {
      return Status::OK();
    }
    const uint32_t length = DecodeFixed32(header.data());
    const uint32_t crc_masked = DecodeFixed32(header.data() + 4);

    std::string payload;
    s = file->Read(length, &payload);
    if (!s.ok()) {
      return s;
    }
    if (payload.size() < length) {
      return Status::OK();  // torn tail
    }

    const uint32_t expected = crc32c::Unmask(crc_masked);
    const uint32_t actual = crc32c::Value(payload.data(), payload.size());
    if (actual != expected) {
      return Status::Corruption("MANIFEST checksum mismatch");
    }

    VersionEdit edit;
    s = edit.DecodeFrom(payload);
    if (!s.ok()) {
      return s;
    }
    s = ApplyEdit(edit, builder);
    if (!s.ok()) {
      return s;
    }
  }
}

Status VersionSet::NewDB() {
  if (!env_->FileExists(dbname_)) {
    Status s = env_->CreateDir(dbname_);
    if (!s.ok()) {
      return s;
    }
  }

  VersionEdit new_db;
  new_db.SetComparatorName(kComparatorName);
  new_db.SetLogNumber(1);
  new_db.SetNextFileNumber(2);
  new_db.SetLastSequence(0);

  const uint64_t manifest_number = 1;
  const std::string manifest_path =
      ManifestFileName(dbname_, manifest_number);

  {
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(manifest_path, &file);
    if (!s.ok()) {
      return s;
    }
    std::string payload;
    new_db.EncodeTo(&payload);
    s = file->Append(EncodeManifestRecord(payload));
    if (!s.ok()) {
      return s;
    }
    s = file->Sync();
    if (!s.ok()) {
      return s;
    }
    s = file->Close();
    if (!s.ok()) {
      return s;
    }
  }

  Status s = SetCurrentFile(manifest_number);
  if (!s.ok()) {
    return s;
  }

  // Empty 1.log ready for append (format.md §9).
  {
    const std::string log_path = LogFileName(dbname_, 1);
    std::unique_ptr<WritableFile> log;
    s = env_->NewWritableFile(log_path, &log);
    if (!s.ok()) {
      return s;
    }
    s = log->Close();
    if (!s.ok()) {
      return s;
    }
  }

  (void)env_->FsyncDir(dbname_);

  current_ = std::make_shared<Version>();
  log_number_ = 1;
  next_file_number_ = 2;
  last_sequence_ = 0;
  manifest_file_number_ = manifest_number;

  return OpenManifestForAppend(manifest_number);
}

Status VersionSet::Recover() {
  std::string manifest_basename;
  Status s = ReadCurrentFile(&manifest_basename);
  if (!s.ok()) {
    return s;
  }

  // Expect MANIFEST-NNNNNN basename; open as dbname/basename.
  std::string manifest_path = dbname_;
  if (!manifest_path.empty() && manifest_path.back() != '/') {
    manifest_path.push_back('/');
  }
  manifest_path.append(manifest_basename);

  // Parse manifest file number from basename "MANIFEST-<digits>".
  static constexpr char kPrefix[] = "MANIFEST-";
  if (manifest_basename.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
    return Status::Corruption("CURRENT points to non-MANIFEST: " +
                              manifest_basename);
  }
  const char* num_str = manifest_basename.c_str() + (sizeof(kPrefix) - 1);
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(num_str, &end, 10);
  if (end == num_str || *end != '\0') {
    return Status::Corruption("bad MANIFEST number in CURRENT: " +
                              manifest_basename);
  }
  manifest_file_number_ = static_cast<uint64_t>(parsed);

  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(manifest_path, &file);
  if (!s.ok()) {
    return s;
  }

  // Reset metadata to empty-base defaults, then apply all edits.
  auto builder = std::make_shared<Version>();
  next_file_number_ = 2;
  last_sequence_ = 0;
  log_number_ = 0;

  s = ReplayManifest(file.get(), builder.get());
  if (!s.ok()) {
    return s;
  }
  file.reset();

  current_ = std::move(builder);
  return OpenManifestForAppend(manifest_file_number_);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  // Ensure next_file_number is persisted when caller did not set it.
  // Callers (flush) should SetNextFileNumber(Peek...); still safe if omitted.
  if (!edit->HasNextFileNumber()) {
    edit->SetNextFileNumber(next_file_number_);
  }

  auto new_version = CloneVersion(*current_);
  Status s = ApplyEdit(*edit, new_version.get());
  if (!s.ok()) {
    return s;
  }

  // Create a fresh MANIFEST only when none is open (should not happen after
  // NewDB/Recover). Educational path: append to the live descriptor.
  const bool created_new_manifest = (descriptor_log_ == nullptr);
  if (created_new_manifest) {
    // Allocate a new manifest file number and write CURRENT after first record.
    const uint64_t new_manifest = NewFileNumber();
    manifest_file_number_ = new_manifest;
    const std::string path = ManifestFileName(dbname_, new_manifest);
    s = env_->NewWritableFile(path, &descriptor_log_);
    if (!s.ok()) {
      return s;
    }
  }

  s = WriteEditToManifest(*edit);
  if (!s.ok()) {
    return s;
  }

  if (created_new_manifest) {
    s = SetCurrentFile(manifest_file_number_);
    if (!s.ok()) {
      return s;
    }
  }

  current_ = std::move(new_version);
  return Status::OK();
}

}  // namespace tinylsm
