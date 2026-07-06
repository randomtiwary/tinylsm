#pragma once

// DBImpl: Open/LOCK, WAL+MemTable write path, multi-log WAL replay, Get.
// No flush / BG worker in this PR (see PR 13).

#include "internal_key.h"
#include "memtable.h"
#include "version_set.h"
#include "wal.h"

#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tinylsm {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  // Full Open algorithm (design §2.4 / Open recovery). Called only from DB::Open.
  Status Open();

  Status Put(const WriteOptions& options, const std::string& key,
             const std::string& value) override;
  Status Delete(const WriteOptions& options, const std::string& key) override;
  Status Get(const ReadOptions& options, const std::string& key,
             std::string* value) override;

 private:
  // Shared write path for Put (kTypeValue) and Delete (kTypeDeletion).
  Status Write(const WriteOptions& options, ValueType type,
               const std::string& key, const std::string& value);

  Status AcquireLock();
  Status MaybeCreateOrRecover();
  Status BumpFileNumbersFromDirectory();
  Status ReplayWalsAndInstallMem();
  Status OpenWalForAppend(uint64_t log_number);

  // SST search for a held Version (L0 newest-first, then L1). Opens tables on demand.
  Status GetFromVersion(const std::shared_ptr<Version>& version,
                        const LookupKey& lkey, std::string* value);

  Status GetFromTableFile(const FileMetaData& meta, const LookupKey& lkey,
                          std::string* value);

  static bool ParseNumberedBasename(const std::string& name,
                                    const std::string& suffix,
                                    uint64_t* number);
  static bool ParseManifestBasename(const std::string& name, uint64_t* number);

  Options options_;
  std::string dbname_;
  Env* env_;

  // Protects memtables, sequence, version pointer reads for mem path, WAL writer.
  std::mutex mutex_;

  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<VersionSet> versions_;

  std::unique_ptr<MemTable> mem_;
  // Reserved for flush PR; always null here.
  std::unique_ptr<MemTable> imm_;

  std::unique_ptr<WalWriter> wal_;
  uint64_t current_log_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  InternalKeyComparator icmp_;
  Status bg_error_;  // sticky error for later BG; unused for writes yet
};

}  // namespace tinylsm
