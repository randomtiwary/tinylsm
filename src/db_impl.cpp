#include "db_impl.h"

#include "table.h"
#include "table_builder.h"
#include "tinylsm/filename.h"

#include <algorithm>
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
  }
  if (db_lock_) {
    (void)env_->UnlockFile(std::move(db_lock_));
  }
  versions_.reset();
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

bool DBImpl::HasBackgroundWorkLocked() const {
  return (imm_ != nullptr) || compaction_scheduled_ || bg_working_;
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
  // Allocate file number under... caller must pass number already, or we
  // allocate here without mutex? Design: allocate under mutex before unlock.
  // We receive meta->number pre-assigned by BG under lock.
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
  std::unique_lock<std::mutex> lock(mutex_);
  while (!shutting_down_ || HasBackgroundWorkLocked()) {
    while (!HasBackgroundWorkLocked() && !shutting_down_) {
      bg_cv_.wait(lock);
    }
    if (shutting_down_ && !HasBackgroundWorkLocked()) {
      break;
    }

    // Crash simulation: abandon work without apply so multi-log remains.
    if (test_simulate_crash_) {
      bg_working_ = false;
      break;
    }

    // Prefer flush over compaction. On clean shutdown with imm_ pending and
    // no bg_error_, still run the flush to completion.
    if (imm_ != nullptr && bg_error_.ok()) {
      std::shared_ptr<MemTable> imm = imm_;  // keep alive across unlock
      const uint64_t imm_log = imm_log_number_;
      (void)imm_log;

      FileMetaData meta;
      meta.number = versions_->NewFileNumber();

      bg_working_ = true;
      lock.unlock();  // ---- SST IO without mutex ----

      Status s = WriteLevel0Table(imm.get(), &meta);

      lock.lock();
      if (test_simulate_crash_) {
        bg_working_ = false;
        background_work_finished_cv_.notify_all();
        break;
      }

      if (!s.ok()) {
        // Delete partial SST; leave WALs for recovery.
        (void)env_->DeleteFile(TableFileName(dbname_, meta.number));
        (void)env_->DeleteFile(TableTempFileName(dbname_, meta.number));
        bg_error_ = s;
        bg_working_ = false;
        background_work_finished_cv_.notify_all();
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
      continue;
    }

    // No flush work (or bg_error_ blocks further flush). If shutting down, exit.
    if (shutting_down_) {
      bg_working_ = false;
      break;
    }
    // Spurious wake or only compaction (unused): wait again.
    if (imm_ == nullptr) {
      bg_cv_.wait(lock);
    }
  }
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
