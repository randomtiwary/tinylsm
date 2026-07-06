#include "db_impl.h"

#include "table.h"
#include "table_builder.h"
#include "tinylsm/filename.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
  (void)options_.sync_writes;
}

DBImpl::~DBImpl() {
  // §10.4 Shutdown: signal BG, join (BG finishes in-flight imm flush if no
  // bg_error_ and not crash-sim), fsync WAL, release LOCK.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    shutting_down_ = true;
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_ && !test_simulate_crash_) {
      (void)wal_->Sync();
      (void)wal_->Close();
    } else if (wal_) {
      // Crash sim: best-effort close without requiring flush complete.
      (void)wal_->Close();
    }
    wal_.reset();
    mem_.reset();
    imm_.reset();
    // Drop live Version so any FileMetaData only held via versions_ can expire.
    // Purge only unlinks numbers in pending_obsolete_sst_ (compaction inputs),
    // never live manifest files that were never registered as obsolete.
    versions_.reset();
    PurgeObsoleteFilesLocked();
  }
  if (db_lock_) {
    (void)env_->UnlockFile(std::move(db_lock_));
  }
}

// ---------------------------------------------------------------------------
// TEST hooks
// ---------------------------------------------------------------------------

bool DBImpl::TEST_WaitForFlush() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (imm_ != nullptr && bg_error_.ok() && !shutting_down_) {
    background_work_finished_cv_.wait(lock);
  }
  return bg_error_.ok() && imm_ == nullptr;
}

bool DBImpl::TEST_WaitForCompaction() {
  std::unique_lock<std::mutex> lock(mutex_);
  auto still_busy = [this]() {
    if (!bg_error_.ok() || shutting_down_) {
      return false;
    }
    if (imm_ != nullptr || bg_working_ || compaction_scheduled_) {
      return true;
    }
    auto v = versions_->current();
    if (v && NeedsCompaction(*v, options_.l0_compaction_trigger)) {
      return true;
    }
    return false;
  };
  while (still_busy()) {
    // Wake BG in case NeedsCompaction but schedule bit not set.
    MaybeScheduleCompactionLocked();
    background_work_finished_cv_.wait_for(lock, std::chrono::milliseconds(50));
  }
  return bg_error_.ok();
}

Status DBImpl::TEST_ForceFreeze() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  if (imm_ != nullptr) {
    // Wait for pending flush then freeze.
    while (imm_ != nullptr && bg_error_.ok()) {
      background_work_finished_cv_.wait(lock);
    }
    if (!bg_error_.ok()) {
      return bg_error_;
    }
  }
  if (mem_ == nullptr) {
    return Status::IOError("no memtable");
  }
  return FreezeMemTableLocked();
}

void DBImpl::TEST_SimulateCrash() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    test_simulate_crash_ = true;
    shutting_down_ = true;
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Leave WALs and any partial/orphan SST on disk; drop in-memory state.
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
  // versions_ kept until destructor; mark crash so ~DBImpl does not re-join.
  bg_thread_started_ = false;
}

void DBImpl::TEST_SetFailBeforeFlushApply(bool v) {
  std::lock_guard<std::mutex> lock(mutex_);
  test_fail_before_flush_apply_ = v;
}

void DBImpl::TEST_SetFailFlushIO(bool v) {
  std::lock_guard<std::mutex> lock(mutex_);
  test_fail_flush_io_ = v;
}

void DBImpl::TEST_SetFailBeforeCompactionApply(bool v) {
  std::lock_guard<std::mutex> lock(mutex_);
  test_fail_before_compaction_apply_ = v;
}

Status DBImpl::TEST_BgError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bg_error_;
}

bool DBImpl::TEST_HasImm() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return imm_ != nullptr;
}

uint64_t DBImpl::TEST_CurrentLogNumber() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_log_number_;
}

uint64_t DBImpl::TEST_ManifestLogNumber() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->LogNumber();
}

uint64_t DBImpl::TEST_PeekNextFileNumber() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->PeekNextFileNumber();
}

size_t DBImpl::TEST_MemApproximateMemoryUsage() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mem_ ? mem_->ApproximateMemoryUsage() : 0;
}

int DBImpl::TEST_NumL0Files() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto v = versions_->current();
  return v ? static_cast<int>(v->NumFiles(0)) : 0;
}

int DBImpl::TEST_NumL1Files() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto v = versions_->current();
  return v ? static_cast<int>(v->NumFiles(1)) : 0;
}

std::shared_ptr<Version> DBImpl::TEST_CurrentVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->current();
}

void DBImpl::TEST_PurgeObsoleteFiles() {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeObsoleteFilesLocked();
}

bool DBImpl::TEST_SstFileExists(uint64_t number) const {
  return env_->FileExists(TableFileName(dbname_, number));
}

// ---------------------------------------------------------------------------
// Open / recovery
// ---------------------------------------------------------------------------

Status DBImpl::AcquireLock() {
  if (!env_->FileExists(dbname_)) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument("DB does not exist: " + dbname_);
    }
    Status s = env_->CreateDir(dbname_);
    if (!s.ok()) {
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

Status DBImpl::OpenNewWalFile(uint64_t log_number) {
  // Create a brand-new empty log file (freeze path).
  const std::string path = LogFileName(dbname_, log_number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(path, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Sync();
  if (!s.ok()) {
    return s;
  }
  // Re-open as appendable for the writer (or just use the writable file).
  wal_ = std::make_unique<WalWriter>(std::move(file));
  current_log_number_ = log_number;
  (void)env_->FsyncDir(dbname_);
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

  auto recovery_mem = std::make_shared<MemTable>(icmp_);
  SequenceNumber wal_max_seq = 0;

  if (logs.empty()) {
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

  const uint64_t highest = logs.back();
  return OpenWalForAppend(highest);
}

Status DBImpl::MaybeFreezeOversizedMemAfterRecovery() {
  // R9 / Open step 9: bound memory after large WAL replay.
  if (mem_ == nullptr) {
    return Status::OK();
  }
  if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
    return Status::OK();
  }
  if (imm_ != nullptr) {
    return Status::OK();  // should not happen on recovery install
  }
  return FreezeMemTableLocked();
}

Status DBImpl::MaybeCreateOrRecover() {
  const std::string current = CurrentFileName(dbname_);
  const bool exists = env_->FileExists(current);

  if (exists && options_.error_if_exists) {
    return Status::InvalidArgument("DB already exists: " + dbname_);
  }

  if (!exists) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(
          "DB does not exist (create_if_missing=false): " + dbname_);
    }
    Status s = versions_->NewDB();
    if (!s.ok()) {
      return s;
    }
    mem_ = std::make_shared<MemTable>(icmp_);
    imm_.reset();
    last_sequence_ = 0;
    versions_->SetLastSequence(0);
    s = OpenWalForAppend(/*log_number=*/1);
    if (!s.ok()) {
      return s;
    }
    return BumpFileNumbersFromDirectory();
  }

  Status s = versions_->Recover();
  if (!s.ok()) {
    return s;
  }
  s = BumpFileNumbersFromDirectory();
  if (!s.ok()) {
    return s;
  }
  s = ReplayWalsAndInstallMem();
  if (!s.ok()) {
    return s;
  }
  return MaybeFreezeOversizedMemAfterRecovery();
}

void DBImpl::StartBackgroundThread() {
  if (bg_thread_started_) {
    return;
  }
  bg_thread_started_ = true;
  bg_thread_ = std::thread([this]() { BackgroundCall(); });
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
  if (mem_ == nullptr || wal_ == nullptr) {
    return Status::Corruption("Open left memtable or WAL unready");
  }
  // Open step 10: start BG (may already have imm_ from R9 post-open freeze).
  StartBackgroundThread();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (imm_ != nullptr) {
      bg_cv_.notify_all();
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Freeze / MakeRoom / write path
// ---------------------------------------------------------------------------

void DBImpl::MaybeScheduleFlushLocked() {
  // Wake BG if there is an imm to flush.
  if (imm_ != nullptr) {
    bg_cv_.notify_all();
  }
}

void DBImpl::MaybeScheduleCompactionLocked() {
  if (!bg_error_.ok() || shutting_down_) {
    return;
  }
  auto v = versions_->current();
  if (v == nullptr) {
    return;
  }
  if (NeedsCompaction(*v, options_.l0_compaction_trigger)) {
    compaction_scheduled_ = true;
    bg_cv_.notify_all();
  }
}

bool DBImpl::HasBackgroundWorkLocked() const {
  // Sticky error: do not treat pending imm_ as runnable work — otherwise the
  // BG loop busy-spins holding mutex_ (deadlocks writers and ~DBImpl). Design
  // §10: fail writers/gets; leave WALs; idle until shutdown.
  if (!bg_error_.ok()) {
    return bg_working_;
  }
  // Also treat "needs compaction but not yet scheduled" as work so shutdown
  // can still finish a pending L0→L1 after the last flush (optional; tests use
  // explicit wait). Prefer scheduling bit for the wait predicate.
  return (imm_ != nullptr) || compaction_scheduled_ || bg_working_;
}

void DBImpl::NoteObsoleteFilesLocked(
    const std::vector<std::shared_ptr<FileMetaData>>& files) {
  for (const auto& f : files) {
    if (f) {
      pending_obsolete_sst_.emplace_back(f->number, f);
    }
  }
  PurgeObsoleteFilesLocked();
}

void DBImpl::PurgeObsoleteFilesLocked() {
  if (pending_obsolete_sst_.empty()) {
    return;
  }
  std::vector<std::pair<uint64_t, std::weak_ptr<FileMetaData>>> still;
  still.reserve(pending_obsolete_sst_.size());
  for (auto& ent : pending_obsolete_sst_) {
    const uint64_t number = ent.first;
    // Expired weak_ptr ⇒ no shared_ptr (no Version) still references this meta.
    if (ent.second.expired()) {
      const std::string path = TableFileName(dbname_, number);
      if (env_->FileExists(path)) {
        Status s = env_->DeleteFile(path);
        if (s.ok()) {
          std::fprintf(stderr,
                       "tinylsm: unlink obsolete sst=%llu\n",
                       static_cast<unsigned long long>(number));
        }
      }
    } else {
      still.push_back(std::move(ent));
    }
  }
  pending_obsolete_sst_ = std::move(still);
}

Status DBImpl::FreezeMemTableLocked() {
  // REQUIRES: mutex_ held; imm_ == nullptr.
  if (imm_ != nullptr) {
    return Status::IOError("FreezeMemTableLocked called with imm pending");
  }
  if (mem_ == nullptr) {
    return Status::IOError("no memtable to freeze");
  }

  // mem_ → imm_; open new empty mem_ + new WAL. Do NOT advance manifest log_number.
  imm_ = mem_;
  imm_log_number_ = current_log_number_;
  mem_ = std::make_shared<MemTable>(icmp_);

  // Close current WAL (data is complete for imm). New file number for active log.
  if (wal_) {
    (void)wal_->Sync();
    (void)wal_->Close();
    wal_.reset();
  }

  const uint64_t new_log = versions_->NewFileNumber();
  Status s = OpenNewWalFile(new_log);
  if (!s.ok()) {
    // Best-effort restore is hard; sticky error.
    bg_error_ = s;
    return s;
  }

  // INFO-style: log numbers only (educational observability).
  std::fprintf(stderr,
               "tinylsm: freeze mem->imm log=%llu new_active_log=%llu\n",
               static_cast<unsigned long long>(imm_log_number_),
               static_cast<unsigned long long>(current_log_number_));

  MaybeScheduleFlushLocked();
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  const size_t limit = options_.write_buffer_size;
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (shutting_down_) {
      return Status::IOError("DB is closing");
    }
    // Write stall: need to freeze but imm_ still pending flush.
    if (imm_ != nullptr && mem_ != nullptr &&
        mem_->ApproximateMemoryUsage() >= limit) {
      background_work_finished_cv_.wait(lock);
      continue;
    }
    // Room to freeze now.
    if (mem_ != nullptr && mem_->ApproximateMemoryUsage() >= limit &&
        imm_ == nullptr) {
      Status s = FreezeMemTableLocked();
      if (!s.ok()) {
        return s;
      }
      // Loop once more (new mem is empty — room for this write).
      continue;
    }
    return Status::OK();
  }
}

Status DBImpl::Write(const WriteOptions& options, ValueType type,
                     const std::string& key, const std::string& value) {
  if (key.size() > options_.max_key_size) {
    return Status::InvalidArgument("key too large");
  }
  if (type == kTypeValue && value.size() > options_.max_value_size) {
    return Status::InvalidArgument("value too large");
  }

  std::unique_lock<std::mutex> lock(mutex_);
  Status s = MakeRoomForWrite(lock);
  if (!s.ok()) {
    return s;
  }
  if (mem_ == nullptr || wal_ == nullptr) {
    return Status::IOError("DB not open for write");
  }

  const SequenceNumber seq = last_sequence_ + 1;
  s = wal_->AddRecord(seq, static_cast<uint8_t>(type), key, value);
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
  // Keep VersionSet high-water in sync for flush edits (global, not SST-only).
  versions_->SetLastSequence(last_sequence_);

  // After write: if mem full and imm free, freeze + schedule (design §4 trigger).
  if (mem_->ApproximateMemoryUsage() >= options_.write_buffer_size &&
      imm_ == nullptr) {
    s = FreezeMemTableLocked();
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, const std::string& key,
                   const std::string& value) {
  return Write(options, kTypeValue, key, value);
}

Status DBImpl::Delete(const WriteOptions& options, const std::string& key) {
  return Write(options, kTypeDeletion, key, std::string());
}

// ---------------------------------------------------------------------------
// BG worker: flush imm → L0
// ---------------------------------------------------------------------------

Status DBImpl::WriteLevel0Table(MemTable* imm, FileMetaData* meta) {
  // meta->number pre-assigned by BG under lock. Mutex must NOT be held (IO).
  const std::string tmp = TableTempFileName(dbname_, meta->number);
  const std::string final_path = TableFileName(dbname_, meta->number);

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(tmp, &file);
  if (!s.ok()) {
    return s;
  }

  TableBuilder builder(file.get(), options_.block_size);
  {
    MemTable::Iterator it(imm);
    it.SeekToFirst();
    for (; it.Valid(); it.Next()) {
      builder.Add(it.key(), it.value());
    }
  }

  TableBuildStats stats;
  s = builder.Finish(&stats);
  if (!s.ok()) {
    (void)file->Close();
    (void)env_->DeleteFile(tmp);
    return s;
  }
  s = file->Sync();
  if (!s.ok()) {
    (void)file->Close();
    (void)env_->DeleteFile(tmp);
    return s;
  }
  s = file->Close();
  if (!s.ok()) {
    (void)env_->DeleteFile(tmp);
    return s;
  }
  s = env_->RenameFile(tmp, final_path);
  if (!s.ok()) {
    (void)env_->DeleteFile(tmp);
    return s;
  }
  (void)env_->FsyncDir(dbname_);

  meta->file_size = stats.file_size;
  meta->smallest = stats.smallest_key;
  meta->largest = stats.largest_key;
  return Status::OK();
}

void DBImpl::RemoveObsoleteLogsLocked(uint64_t old_log_number,
                                      uint64_t new_log_number) {
  // Delete K.log for K in [old_log_number, new_log_number) when K < new and
  // K != current_log_number_ (never delete the open active log).
  if (new_log_number <= old_log_number) {
    return;
  }
  std::vector<std::string> children;
  if (!env_->GetChildren(dbname_, &children).ok()) {
    return;
  }
  for (const auto& name : children) {
    uint64_t n = 0;
    if (!ParseNumberedBasename(name, ".log", &n)) {
      continue;
    }
    if (n < new_log_number && n != current_log_number_) {
      (void)env_->DeleteFile(LogFileName(dbname_, n));
    }
  }
}

void DBImpl::BackgroundCall() {
  // §10.3 BGMain
  //
  // Lock protocol: hold mutex_ only for scheduling / version publish / flags.
  // Never tight-loop while holding mutex_ (see sticky bg_error_ idle path).
  std::unique_lock<std::mutex> lock(mutex_);
  while (!shutting_down_ || HasBackgroundWorkLocked()) {
    while (!HasBackgroundWorkLocked() && !shutting_down_) {
      bg_cv_.wait(lock);  // releases mutex_ while idle
    }
    if (shutting_down_ && !HasBackgroundWorkLocked()) {
      break;
    }

    // Crash simulation: abandon work without apply so multi-log remains.
    if (test_simulate_crash_) {
      bg_working_ = false;
      background_work_finished_cv_.notify_all();
      break;
    }

    // Sticky bg_error_: stop flush attempts. imm_ may still be set (WALs hold
    // data for recovery). Idle on cv until shutdown — do NOT spin on mutex_.
    if (!bg_error_.ok()) {
      bg_working_ = false;
      background_work_finished_cv_.notify_all();
      if (shutting_down_) {
        break;
      }
      // Wait for shutdown signal (or a future clear-error policy). Releases mutex_.
      bg_cv_.wait(lock);
      continue;
    }

    // Prefer flush over compaction. On clean shutdown with imm_ pending and
    // no bg_error_, still run the flush to completion.
    if (imm_ != nullptr) {
      std::shared_ptr<MemTable> imm = imm_;  // keep alive across unlock
      const uint64_t imm_log = imm_log_number_;
      (void)imm_log;

      FileMetaData meta;
      meta.number = versions_->NewFileNumber();
      // Snapshot TEST inject under lock (WriteLevel0Table runs unlocked).
      const bool inject_io_fail = test_fail_flush_io_;

      bg_working_ = true;
      lock.unlock();  // ---- SST IO without mutex ----

      Status s = inject_io_fail
                     ? Status::IOError("TEST: injected WriteLevel0Table failure")
                     : WriteLevel0Table(imm.get(), &meta);

      lock.lock();
      if (test_simulate_crash_) {
        bg_working_ = false;
        background_work_finished_cv_.notify_all();
        break;
      }

      if (!s.ok()) {
        // Delete partial SST; leave WALs + imm_ for recovery; sticky error.
        (void)env_->DeleteFile(TableFileName(dbname_, meta.number));
        (void)env_->DeleteFile(TableTempFileName(dbname_, meta.number));
        bg_error_ = s;
        bg_working_ = false;
        // Wake write-stall waiters so they observe bg_error_ and fail Put.
        background_work_finished_cv_.notify_all();
        // Fall through to sticky-error idle path on next iteration (no spin).
        continue;
      }

      if (test_fail_before_flush_apply_) {
        // R3/R4 path: leave durable orphan SST on disk; do not LogAndApply.
        // Clear imm so writers are not stuck forever; data remains in WALs.
        // (Crash-before-apply leaves imm only if process dies; for inject we
        // drop imm and rely on multi-log recovery.)
        std::fprintf(stderr,
                     "tinylsm: TEST fail-before-apply orphan sst=%llu\n",
                     static_cast<unsigned long long>(meta.number));
        // Keep orphan on disk intentionally (R4). Drop imm; WALs still hold data.
        imm_.reset();
        imm_log_number_ = 0;
        bg_working_ = false;
        background_work_finished_cv_.notify_all();
        // One-shot inject so subsequent flushes can succeed if needed.
        test_fail_before_flush_apply_ = false;
        continue;
      }

      // After durable SST: LogAndApply under mutex.
      const uint64_t old_log = versions_->LogNumber();
      VersionEdit edit;
      edit.AddFile(/*level=*/0, meta.number, meta.file_size, meta.smallest,
                   meta.largest);
      // Active log after freeze; recovery replays it fully.
      edit.SetLogNumber(current_log_number_);
      edit.SetNextFileNumber(versions_->PeekNextFileNumber());  // getter only
      edit.SetLastSequence(last_sequence_);  // GLOBAL high-water

      s = versions_->LogAndApply(&edit);
      if (!s.ok()) {
        // Retain imm_; sticky error; idle (same as WriteLevel0Table failure).
        bg_error_ = s;
        bg_working_ = false;
        background_work_finished_cv_.notify_all();
        continue;
      }

      std::fprintf(stderr,
                   "tinylsm: flush L0 file=%llu size=%llu log_number=%llu\n",
                   static_cast<unsigned long long>(meta.number),
                   static_cast<unsigned long long>(meta.file_size),
                   static_cast<unsigned long long>(versions_->LogNumber()));

      RemoveObsoleteLogsLocked(old_log, versions_->LogNumber());
      imm_.reset();
      imm_log_number_ = 0;
      bg_working_ = false;
      background_work_finished_cv_.notify_all();
      MaybeScheduleCompactionLocked();
      PurgeObsoleteFilesLocked();
      continue;
    }

    // Prefer flush over compaction. Run compaction when scheduled or needed.
    if (compaction_scheduled_ ||
        (versions_->current() &&
         NeedsCompaction(*versions_->current(),
                         options_.l0_compaction_trigger))) {
      BackgroundCompaction(lock);
      continue;
    }

    // No imm / compaction and no error: shutdown exit or wait for more work.
    if (shutting_down_) {
      bg_working_ = false;
      break;
    }
    // Spurious wake / no runnable work: wait (always releases mutex_).
    bg_cv_.wait(lock);
  }
}

void DBImpl::BackgroundCompaction(std::unique_lock<std::mutex>& lock) {
  // REQUIRES: mutex_ held. Prefer caller already checked needs/scheduled.
  compaction_scheduled_ = false;

  if (!bg_error_.ok() || versions_->current() == nullptr) {
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    return;
  }

  CompactionInputs inputs =
      PickCompaction(*versions_->current(), options_.l0_compaction_trigger);
  if (inputs.empty()) {
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    return;
  }

  // Hold input metas across apply for obsolete-file bookkeeping (C3).
  std::vector<std::shared_ptr<FileMetaData>> obsolete;
  obsolete.reserve(inputs.level0.size() + inputs.level1.size());
  for (const auto& f : inputs.level0) {
    obsolete.push_back(f);
  }
  for (const auto& f : inputs.level1) {
    obsolete.push_back(f);
  }

  FileMetaData output;
  output.number = versions_->NewFileNumber();
  const bool inject_skip_apply = test_fail_before_compaction_apply_;

  bg_working_ = true;
  lock.unlock();  // ---- compaction IO without mutex ----

  bool wrote_output = false;
  Status s = DoCompactionWork(env_, dbname_, options_, inputs, &output,
                              &wrote_output);

  lock.lock();
  if (test_simulate_crash_) {
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    return;
  }

  if (!s.ok()) {
    (void)env_->DeleteFile(TableFileName(dbname_, output.number));
    (void)env_->DeleteFile(TableTempFileName(dbname_, output.number));
    bg_error_ = s;
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    return;
  }

  if (inject_skip_apply) {
    // R6: leave durable orphan output SST (if any); inputs remain in manifest.
    std::fprintf(stderr,
                 "tinylsm: TEST fail-before-compaction-apply orphan sst=%llu "
                 "wrote=%d\n",
                 static_cast<unsigned long long>(output.number),
                 wrote_output ? 1 : 0);
    test_fail_before_compaction_apply_ = false;
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    // Do not clear compaction need forever — L0 still over trigger; avoid
    // tight loop by leaving scheduled false until next flush schedules again.
    // Re-schedule so a later run can succeed if inject was one-shot.
    MaybeScheduleCompactionLocked();
    return;
  }

  VersionEdit edit;
  for (const auto& f : inputs.level0) {
    edit.DeleteFile(/*level=*/0, f->number);
  }
  for (const auto& f : inputs.level1) {
    edit.DeleteFile(/*level=*/1, f->number);
  }
  if (wrote_output) {
    edit.AddFile(/*level=*/1, output.number, output.file_size, output.smallest,
                 output.largest);
  }
  // Compaction-only edit: do not change log_number (design §6).
  edit.SetNextFileNumber(versions_->PeekNextFileNumber());
  edit.SetLastSequence(last_sequence_);

  s = versions_->LogAndApply(&edit);
  if (!s.ok()) {
    bg_error_ = s;
    bg_working_ = false;
    background_work_finished_cv_.notify_all();
    return;
  }

  const size_t n_l0_in = inputs.level0.size();
  const size_t n_l1_in = inputs.level1.size();
  std::fprintf(stderr,
               "tinylsm: compact L0->L1 out=%llu size=%llu l0_in=%zu l1_in=%zu "
               "wrote=%d\n",
               static_cast<unsigned long long>(output.number),
               static_cast<unsigned long long>(output.file_size), n_l0_in,
               n_l1_in, wrote_output ? 1 : 0);

  // Inputs no longer in current Version; unlink when no reader holds them.
  // Register weak_ptrs, then drop *all* job-local strong refs (obsolete vector
  // and CompactionInputs) before Purge — otherwise weak_ptrs never expire in
  // this function and the last compaction's inputs leak until a later purge
  // or process exit (review Bug 1).
  NoteObsoleteFilesLocked(obsolete);
  obsolete.clear();
  inputs.level0.clear();
  inputs.level1.clear();
  PurgeObsoleteFilesLocked();
  bg_working_ = false;
  background_work_finished_cv_.notify_all();
  // Another compaction may still be needed if trigger still exceeded (unlikely
  // after full L0 drain) or new flushes arrived.
  MaybeScheduleCompactionLocked();
}

// ---------------------------------------------------------------------------
// Get path (§10.1) — mem/imm under lock with Found/Deleted distinction
// ---------------------------------------------------------------------------

bool DBImpl::GetFromTableFile(const FileMetaData& meta, const LookupKey& lkey,
                              std::string* value, Status* s) {
  const std::string path = TableFileName(dbname_, meta.number);
  std::unique_ptr<RandomAccessFile> file;
  Status st = env_->NewRandomAccessFile(path, &file);
  if (!st.ok()) {
    // Missing file that is in the manifest is a hard error.
    *s = st;
    return true;
  }
  uint64_t file_size = meta.file_size;
  if (file_size == 0) {
    st = env_->GetFileSize(path, &file_size);
    if (!st.ok()) {
      *s = st;
      return true;
    }
  }
  std::unique_ptr<Table> table;
  st = Table::Open(file.get(), file_size, &table);
  if (!st.ok()) {
    *s = st;
    return true;
  }
  return table->Get(lkey.internal_key(), value, s);
}

Status DBImpl::GetFromVersion(const std::shared_ptr<Version>& version,
                              const LookupKey& lkey, std::string* value) {
  const std::string_view user_key = lkey.user_key();

  // L0: newest first (higher file number). Stop on Found or Deleted.
  const auto& l0 = version->LevelFiles(0);
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    Status s;
    if (GetFromTableFile(**it, lkey, value, &s)) {
      return s;  // OK, NotFound(deleted), or hard error
    }
  }

  // L1: non-overlapping ranges; stop on Found or Deleted for a matching file.
  const auto& l1 = version->LevelFiles(1);
  for (const auto& f : l1) {
    if (!UserKeyInFileRange(user_key, *f)) {
      continue;
    }
    Status s;
    if (GetFromTableFile(*f, lkey, value, &s)) {
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
  Status s;

  // Active memtable under lock — true on value OR deletion tombstone.
  if (mem_->Get(lkey, value, &s)) {
    return s;
  }

  // Immutable memtable under lock.
  if (imm_ != nullptr) {
    if (imm_->Get(lkey, value, &s)) {
      return s;
    }
  }

  // Hold Version via shared_ptr; SST IO without mutex (§10.1).
  std::shared_ptr<Version> version = versions_->current();
  lock.unlock();

  if (version == nullptr) {
    return Status::NotFound(std::string());
  }
  if (version->NumFiles(0) == 0 && version->NumFiles(1) == 0) {
    return Status::NotFound(std::string());
  }
  return GetFromVersion(version, lkey, value);
}

}  // namespace tinylsm
