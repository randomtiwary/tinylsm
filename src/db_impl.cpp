#include "db_impl.h"

#include "table.h"
#include "tinylsm/filename.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace tinylsm {
namespace {

// True if user_key is within [smallest, largest] user-key ranges of an SST.
bool UserKeyInFileRange(std::string_view user_key, const FileMetaData& meta) {
  const std::string_view smallest_uk = ExtractUserKey(meta.smallest);
  const std::string_view largest_uk = ExtractUserKey(meta.largest);
  if (user_key < smallest_uk) {
    return false;
  }
  if (user_key > largest_uk) {
    return false;
  }
  return true;
}

}  // namespace

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;
  if (name.empty()) {
    return Status::InvalidArgument("DB::Open: empty db name");
  }
  auto* impl = new DBImpl(options, name);
  Status s = impl->Open();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *dbptr = impl;
  return Status::OK();
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : options_(options),
      dbname_(dbname),
      env_(options.env != nullptr ? options.env : Env::Default()),
      versions_(std::make_unique<VersionSet>(dbname, env_)) {
  // Honor Options::sync_writes as the WriteOptions default only at call sites;
  // public WriteOptions still defaults to true independently.
  (void)options_.sync_writes;
}

DBImpl::~DBImpl() {
  // Close WAL first (best-effort), then release LOCK.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_) {
      (void)wal_->Close();
      wal_.reset();
    }
    mem_.reset();
    imm_.reset();
  }
  if (db_lock_) {
    (void)env_->UnlockFile(std::move(db_lock_));
  }
  versions_.reset();
}

Status DBImpl::AcquireLock() {
  // Ensure directory exists before creating LOCK (especially create_if_missing).
  if (!env_->FileExists(dbname_)) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument("DB does not exist: " + dbname_);
    }
    Status s = env_->CreateDir(dbname_);
    if (!s.ok()) {
      // Concurrent create is fine if the dir now exists.
      if (!env_->FileExists(dbname_)) {
        return s;
      }
    }
  }

  const std::string lock_path = LockFileName(dbname_);
  Status s = env_->LockFile(lock_path, &db_lock_);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

bool DBImpl::ParseNumberedBasename(const std::string& name,
                                   const std::string& suffix,
                                   uint64_t* number) {
  if (name.size() <= suffix.size()) {
    return false;
  }
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  const std::string num_part = name.substr(0, name.size() - suffix.size());
  if (num_part.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long long v = std::strtoull(num_part.c_str(), &end, 10);
  if (end == num_part.c_str() || *end != '\0') {
    return false;
  }
  *number = static_cast<uint64_t>(v);
  return true;
}

bool DBImpl::ParseManifestBasename(const std::string& name, uint64_t* number) {
  static constexpr char kPrefix[] = "MANIFEST-";
  if (name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
    return false;
  }
  const char* num_str = name.c_str() + (sizeof(kPrefix) - 1);
  if (*num_str == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long v = std::strtoull(num_str, &end, 10);
  if (end == num_str || *end != '\0') {
    return false;
  }
  *number = static_cast<uint64_t>(v);
  return true;
}

Status DBImpl::BumpFileNumbersFromDirectory() {
  std::vector<std::string> children;
  Status s = env_->GetChildren(dbname_, &children);
  if (!s.ok()) {
    return s;
  }
  uint64_t max_found = 0;
  bool any = false;
  for (const auto& name : children) {
    uint64_t n = 0;
    if (ParseNumberedBasename(name, ".log", &n) ||
        ParseNumberedBasename(name, ".sst", &n) ||
        ParseNumberedBasename(name, ".sst.tmp", &n) ||
        ParseNumberedBasename(name, ".dbtmp", &n) ||
        ParseManifestBasename(name, &n)) {
      any = true;
      if (n > max_found) {
        max_found = n;
      }
    }
  }
  if (any) {
    versions_->MarkFileNumberUsed(max_found);
  }
  return Status::OK();
}

Status DBImpl::OpenWalForAppend(uint64_t log_number) {
  const std::string path = LogFileName(dbname_, log_number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewAppendableFile(path, &file);
  if (!s.ok()) {
    return s;
  }
  wal_ = std::make_unique<WalWriter>(std::move(file));
  current_log_number_ = log_number;
  return Status::OK();
}

Status DBImpl::ReplayWalsAndInstallMem() {
  // Collect log numbers N where N.log exists and N >= manifest log_number.
  const uint64_t min_log = versions_->LogNumber();
  std::vector<std::string> children;
  Status s = env_->GetChildren(dbname_, &children);
  if (!s.ok()) {
    return s;
  }
  std::vector<uint64_t> logs;
  for (const auto& name : children) {
    uint64_t n = 0;
    if (ParseNumberedBasename(name, ".log", &n) && n >= min_log) {
      logs.push_back(n);
    }
  }
  std::sort(logs.begin(), logs.end());
  logs.erase(std::unique(logs.begin(), logs.end()), logs.end());

  auto recovery_mem = std::make_unique<MemTable>(icmp_);
  SequenceNumber wal_max_seq = 0;

  if (logs.empty()) {
    // Allocate a fresh log and leave mem empty.
    const uint64_t n = versions_->NewFileNumber();
    {
      std::unique_ptr<WritableFile> file;
      s = env_->NewWritableFile(LogFileName(dbname_, n), &file);
      if (!s.ok()) {
        return s;
      }
      s = file->Close();
      if (!s.ok()) {
        return s;
      }
    }
    (void)env_->FsyncDir(dbname_);
    mem_ = std::move(recovery_mem);
    imm_.reset();
    last_sequence_ = versions_->LastSequence();
    return OpenWalForAppend(n);
  }

  // Always apply every complete record (design step 8 / R0b). Never skip by
  // record.seq <= manifest last_sequence.
  for (uint64_t n : logs) {
    std::unique_ptr<SequentialFile> file;
    s = env_->NewSequentialFile(LogFileName(dbname_, n), &file);
    if (!s.ok()) {
      return s;
    }
    WalReader reader(std::move(file));
    for (;;) {
      SequenceNumber seq = 0;
      uint8_t type = 0;
      std::string key;
      std::string value;
      bool eof = false;
      s = reader.ReadRecord(&seq, &type, &key, &value, &eof);
      if (!s.ok()) {
        return s;
      }
      if (eof) {
        break;
      }
      if (type != kTypeValue && type != kTypeDeletion) {
        return Status::Corruption("WAL record has unknown type");
      }
      recovery_mem->Add(seq, static_cast<ValueType>(type), key, value);
      if (seq > wal_max_seq) {
        wal_max_seq = seq;
      }
    }
  }

  const SequenceNumber manifest_seq = versions_->LastSequence();
  last_sequence_ = std::max(manifest_seq, wal_max_seq);
  versions_->SetLastSequence(last_sequence_);

  mem_ = std::move(recovery_mem);
  imm_.reset();

  // Append to highest existing log >= manifest log_number (recommended).
  const uint64_t highest = logs.back();
  return OpenWalForAppend(highest);
}

Status DBImpl::MaybeCreateOrRecover() {
  const std::string current = CurrentFileName(dbname_);
  const bool exists = env_->FileExists(current);

  if (exists && options_.error_if_exists) {
    return Status::InvalidArgument("DB already exists: " + dbname_);
  }

  if (!exists) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument("DB does not exist (create_if_missing=false): " +
                                     dbname_);
    }
    Status s = versions_->NewDB();
    if (!s.ok()) {
      return s;
    }
    // In-process init mandatory: mem_ + WalWriter ready (no lazy init).
    mem_ = std::make_unique<MemTable>(icmp_);
    imm_.reset();
    last_sequence_ = 0;
    versions_->SetLastSequence(0);
    // Directory already has empty 1.log from NewDB; open for append.
    s = OpenWalForAppend(/*log_number=*/1);
    if (!s.ok()) {
      return s;
    }
    // Still bump allocator from dir in case of unexpected extra files.
    return BumpFileNumbersFromDirectory();
  }

  // Existing DB: recover MANIFEST, bump numbers, replay WALs.
  Status s = versions_->Recover();
  if (!s.ok()) {
    return s;
  }
  s = BumpFileNumbersFromDirectory();
  if (!s.ok()) {
    return s;
  }
  return ReplayWalsAndInstallMem();
}

Status DBImpl::Open() {
  Status s = AcquireLock();
  if (!s.ok()) {
    return s;
  }
  s = MaybeCreateOrRecover();
  if (!s.ok()) {
    return s;
  }
  // Invariant: mem_ non-null, WalWriter open, LOCK held. No BG in this PR.
  if (mem_ == nullptr || wal_ == nullptr) {
    return Status::Corruption("Open left memtable or WAL unready");
  }
  return Status::OK();
}

Status DBImpl::Write(const WriteOptions& options, ValueType type,
                     const std::string& key, const std::string& value) {
  if (key.size() > options_.max_key_size) {
    return Status::InvalidArgument("key too large");
  }
  if (type == kTypeValue && value.size() > options_.max_value_size) {
    return Status::InvalidArgument("value too large");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  if (mem_ == nullptr || wal_ == nullptr) {
    return Status::IOError("DB not open for write");
  }

  // No MakeRoomForWrite / freeze in this PR (flush lands in PR 13).
  const SequenceNumber seq = last_sequence_ + 1;
  Status s = wal_->AddRecord(seq, static_cast<uint8_t>(type), key, value);
  if (!s.ok()) {
    return s;
  }
  if (options.sync) {
    s = wal_->Sync();
    if (!s.ok()) {
      return s;
    }
  }

  mem_->Add(seq, type, key, value);
  last_sequence_ = seq;
  versions_->SetLastSequence(last_sequence_);
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, const std::string& key,
                   const std::string& value) {
  return Write(options, kTypeValue, key, value);
}

Status DBImpl::Delete(const WriteOptions& options, const std::string& key) {
  return Write(options, kTypeDeletion, key, std::string());
}

Status DBImpl::GetFromTableFile(const FileMetaData& meta, const LookupKey& lkey,
                                std::string* value) {
  const std::string path = TableFileName(dbname_, meta.number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(path, &file);
  if (!s.ok()) {
    return s;
  }
  uint64_t file_size = meta.file_size;
  if (file_size == 0) {
    s = env_->GetFileSize(path, &file_size);
    if (!s.ok()) {
      return s;
    }
  }
  std::unique_ptr<Table> table;
  s = Table::Open(file.get(), file_size, &table);
  if (!s.ok()) {
    return s;
  }
  return table->Get(lkey.internal_key(), value);
}

Status DBImpl::GetFromVersion(const std::shared_ptr<Version>& version,
                              const LookupKey& lkey, std::string* value) {
  const std::string_view user_key = lkey.user_key();

  // L0: newest first (higher file number). Files may overlap; search all, reverse order.
  const auto& l0 = version->LevelFiles(0);
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    Status s = GetFromTableFile(**it, lkey, value);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }

  // L1: non-overlapping ranges sorted by smallest user key; linear scan for v1.
  const auto& l1 = version->LevelFiles(1);
  for (const auto& f : l1) {
    if (!UserKeyInFileRange(user_key, *f)) {
      continue;
    }
    Status s = GetFromTableFile(*f, lkey, value);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }

  return Status::NotFound(std::string());
}

Status DBImpl::Get(const ReadOptions& /*options*/, const std::string& key,
                   std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("Get: null value out-param");
  }
  value->clear();

  std::unique_lock<std::mutex> lock(mutex_);
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  if (mem_ == nullptr) {
    return Status::IOError("DB not open for read");
  }

  const SequenceNumber snap = last_sequence_;
  const LookupKey lkey(key, snap);

  // Active memtable under lock.
  Status s = mem_->Get(lkey, value);
  if (s.ok()) {
    return s;
  }
  if (!s.IsNotFound()) {
    return s;
  }

  // Immutable memtable under lock (null until flush PR).
  if (imm_ != nullptr) {
    s = imm_->Get(lkey, value);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }

  // Hold Version via shared_ptr; SST IO without mutex (§10.1).
  std::shared_ptr<Version> version = versions_->current();
  lock.unlock();

  if (version == nullptr) {
    return Status::NotFound(std::string());
  }
  // Fast path when levels empty (typical for this PR).
  if (version->NumFiles(0) == 0 && version->NumFiles(1) == 0) {
    return Status::NotFound(std::string());
  }
  return GetFromVersion(version, lkey, value);
}

}  // namespace tinylsm
