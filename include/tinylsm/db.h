#pragma once

// Public DB interface for TinyLSM (Put / Get / Delete).

#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <string>

namespace tinylsm {

class DB {
 public:
  // Open (or create) a database at "name". On success, *dbptr owns a heap
  // allocated DB that the caller must delete. Acquires the directory LOCK;
  // fails if the lock is held or Options reject the path state.
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);

  // Destructor == Close: release LOCK, close WAL, free in-memory state.
  // No separate Close() in v1.
  virtual ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& options, const std::string& key,
                     const std::string& value) = 0;
  virtual Status Delete(const WriteOptions& options,
                        const std::string& key) = 0;
  virtual Status Get(const ReadOptions& options, const std::string& key,
                     std::string* value) = 0;

 protected:
  DB() = default;
};

}  // namespace tinylsm
