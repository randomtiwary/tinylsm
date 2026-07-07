#pragma once

// Filesystem and time abstraction (LevelDB-style Env subset).
// POSIX implementation lives in src/env_posix.cpp.
// Educational clarity preferred over micro-optimizations.
// SST random reads use pread (not mmap) for TSan-friendly explicit IO.

#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

// Sequential append-only writer (WAL, SST builder, MANIFEST).
class WritableFile {
 public:
  WritableFile() = default;
  virtual ~WritableFile();

  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  // Append data to the end of the file.
  virtual Status Append(std::string_view data) = 0;

  // Flush data to the OS and fdatasync/fsync for durability.
  virtual Status Sync() = 0;

  // Close the file. Safe to call more than once; subsequent ops fail.
  virtual Status Close() = 0;
};

// Sequential reader (WAL replay, MANIFEST scan).
class SequentialFile {
 public:
  SequentialFile() = default;
  virtual ~SequentialFile();

  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  // Read up to n bytes into *result. OK with empty/shorter result means EOF.
  virtual Status Read(size_t n, std::string* result) = 0;

  // Skip n bytes from the current position. Skipping past EOF is OK.
  virtual Status Skip(uint64_t n) = 0;
};

// Random-access reader (SSTable blocks). Uses pread under the hood.
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile();

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Read up to n bytes starting at offset. Shorter result is OK at EOF.
  virtual Status Read(uint64_t offset, size_t n, std::string* result) const = 0;
};

// Opaque handle for an exclusive directory/file lock (LOCK file).
class FileLock {
 public:
  FileLock() = default;
  virtual ~FileLock();

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};

// Platform filesystem/time interface. Use Env::Default() for the process-wide
// POSIX implementation.
class Env {
 public:
  Env() = default;
  virtual ~Env();

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Process-wide default Env (POSIX). Never delete the returned pointer.
  static Env* Default();

  // Create/truncate fname and open for sequential writes.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;

  // Open existing file for sequential reads from offset 0.
  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;

  // Open existing file for random reads (pread).
  virtual Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) = 0;

  // True if a file or directory exists at fname.
  virtual bool FileExists(const std::string& fname) = 0;

  // List non-directory entries' basenames in dir (not recursive).
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;

  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status DeleteDir(const std::string& dirname) = 0;
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;

  // Get file size in bytes. NotFound/IOError on failure.
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;

  // Acquire an exclusive lock on fname (typically dbname/LOCK).
  // Fails if another process holds the lock, or this process already holds it.
  virtual Status LockFile(const std::string& fname,
                          std::unique_ptr<FileLock>* lock) = 0;

  // Release a lock previously returned by LockFile. lock is consumed.
  virtual Status UnlockFile(std::unique_ptr<FileLock> lock) = 0;

  // Best-effort fsync of a directory (durability of renames/creates).
  virtual Status FsyncDir(const std::string& dirname) = 0;

  // Create a unique temporary directory under /tmp (for tests).
  // *path is set to the created directory path on success.
  virtual Status NewTempDir(std::string* path) = 0;

  // Current time in microseconds since some fixed epoch (monotonic for delays
  // is not required; wall-clock is fine for educational use).
  virtual uint64_t NowMicros() = 0;
};

}  // namespace tinylsm
