#pragma once

// DBImpl: Open/LOCK, WAL+MemTable write path, multi-log WAL replay, Get,
// immutable memtable freeze, single BG worker flush to L0 (design §2.4 / §10).

#include "internal_key.h"
#include "memtable.h"
#include "version_set.h"
#include "wal.h"

#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tinylsm {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  // Full Open algorithm (design §2.4). Called only from DB::Open.
  Status Open();

  Status Put(const WriteOptions& options, const std::string& key,
             const std::string& value) override;
  Status Delete(const WriteOptions& options, const std::string& key) override;
  Status Get(const ReadOptions& options, const std::string& key,
             std::string* value) override;

  // ---- TEST hooks (educational recovery / flush matrix) ----
  // Wait until imm_ is null (flush applied or no imm). Returns false on bg_error_.
  bool TEST_WaitForFlush();
  // Force freeze of current mem if non-empty and imm_ is free; schedule flush.
  Status TEST_ForceFreeze();
  // Simulate process crash: abandon in-flight flush, do not LogAndApply, release LOCK.
  // After this call the DBImpl must not be used; caller should delete it.
  void TEST_SimulateCrash();
  // If true, BG builds SST then fails before LogAndApply (leaves orphan SST + multi-log).
  void TEST_SetFailBeforeFlushApply(bool v);
  // If true, WriteLevel0Table fails immediately; sticky bg_error_ and imm_ retained
  // (production flush-IO failure path — must not hang BG / destructor).
  void TEST_SetFailFlushIO(bool v);
  // Sticky BG error (empty if OK).
  Status TEST_BgError() const;
  bool TEST_HasImm() const;
  uint64_t TEST_CurrentLogNumber() const;
  uint64_t TEST_ManifestLogNumber() const;
  uint64_t TEST_PeekNextFileNumber() const;
  size_t TEST_MemApproximateMemoryUsage() const;
  int TEST_NumL0Files() const;

 private:
  // Shared write path for Put (kTypeValue) and Delete (kTypeDeletion).
  Status Write(const WriteOptions& options, ValueType type,
               const std::string& key, const std::string& value);

  // §10.2: write stall while imm pending and mem full; freeze when room.
  // REQUIRES: mutex_ held (unique_lock).
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  // Freeze mem_ → imm_, new empty mem_, new WAL via NewFileNumber().
  // Does NOT advance manifest log_number. Schedules BG work.
  // REQUIRES: mutex_ held; imm_ == nullptr.
  Status FreezeMemTableLocked();

  // Schedule / wake BG worker. REQUIRES: mutex_ held.
  void MaybeScheduleFlushLocked();

  // BG worker main loop (§10.3).
  void BackgroundCall();

  // has_work for the BG run loop. Sticky bg_error_ means no further flush/compaction
  // attempts count as work (imm may still be non-null for recovery; do not thrash).
  // bg_working_ still counts so an in-flight claim can finish its error path.
  bool HasBackgroundWorkLocked() const;

  // Build L0 SST from imm memtable. Mutex must NOT be held (IO off-lock).
  // On success *meta is filled and file is durable at TableFileName.
  Status WriteLevel0Table(MemTable* imm, FileMetaData* meta);

  // After successful LogAndApply that advanced log_number: delete obsolete *.log.
  // REQUIRES: mutex_ held.
  void RemoveObsoleteLogsLocked(uint64_t old_log_number, uint64_t new_log_number);

  Status AcquireLock();
  Status MaybeCreateOrRecover();
  Status BumpFileNumbersFromDirectory();
  Status ReplayWalsAndInstallMem();
  Status OpenWalForAppend(uint64_t log_number);
  Status OpenNewWalFile(uint64_t log_number);

  // Post-recovery: if mem oversized, freeze and schedule flush (R9 / Open step 9).
  // REQUIRES: mutex_ held after install.
  Status MaybeFreezeOversizedMemAfterRecovery();

  // Start BG thread once. Safe if already started.
  void StartBackgroundThread();

  // SST search for a held Version (L0 newest-first, then L1). Opens tables on demand.
  Status GetFromVersion(const std::shared_ptr<Version>& version,
                        const LookupKey& lkey, std::string* value);

  // Returns true if this file answered the lookup (value or deletion or hard error).
  bool GetFromTableFile(const FileMetaData& meta, const LookupKey& lkey,
                        std::string* value, Status* s);

  static bool ParseNumberedBasename(const std::string& name,
                                    const std::string& suffix,
                                    uint64_t* number);
  static bool ParseManifestBasename(const std::string& name, uint64_t* number);

  Options options_;
  std::string dbname_;
  Env* env_;

  // ---- Single lock authority (§10): protects memtables, sequences, version
  // publish coordination, WAL writer, BG scheduling flags. Never hold across
  // SST build IO; use unique_lock + unlock for WriteLevel0Table. ----
  mutable std::mutex mutex_;
  std::condition_variable bg_cv_;                   // BG wait for work / shutdown
  std::condition_variable background_work_finished_cv_;  // writers: write stall

  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<VersionSet> versions_;

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // at most one; null when no pending flush
  uint64_t imm_log_number_ = 0;    // log that holds imm_ records (informational)

  std::unique_ptr<WalWriter> wal_;
  uint64_t current_log_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  InternalKeyComparator icmp_;
  Status bg_error_;  // sticky; fails subsequent writes and Gets

  // BG worker state (under mutex_ except shutting_down_ read as atomic-ish via mutex).
  bool shutting_down_ = false;
  bool bg_working_ = false;
  bool compaction_scheduled_ = false;  // reserved PR14; always false here
  bool bg_thread_started_ = false;
  std::thread bg_thread_;

  // TEST: abandon flush on shutdown / skip apply after SST write / fail SST IO.
  bool test_simulate_crash_ = false;
  bool test_fail_before_flush_apply_ = false;
  bool test_fail_flush_io_ = false;
};

}  // namespace tinylsm
