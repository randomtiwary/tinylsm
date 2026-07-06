#include "tinylsm/env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace tinylsm {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Status PosixError(const std::string& context, int err_number) {
  return Status::IOError(context + ": " + std::strerror(err_number));
}

// ---------------------------------------------------------------------------
// WritableFile
// ---------------------------------------------------------------------------

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string filename, int fd)
      : filename_(std::move(filename)), fd_(fd) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      // Best-effort close; callers should Close() for checked errors.
      ::close(fd_);
      fd_ = -1;
    }
  }

  Status Append(std::string_view data) override {
    if (fd_ < 0) {
      return Status::IOError(filename_ + ": file already closed");
    }
    const char* src = data.data();
    size_t left = data.size();
    while (left > 0) {
      const ssize_t done = ::write(fd_, src, left);
      if (done < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      src += done;
      left -= static_cast<size_t>(done);
    }
    return Status::OK();
  }

  Status Sync() override {
    if (fd_ < 0) {
      return Status::IOError(filename_ + ": file already closed");
    }
#if defined(__APPLE__)
    if (::fsync(fd_) != 0) {
      return PosixError("fsync " + filename_, errno);
    }
#else
    // Prefer fdatasync when available (Linux): metadata-only updates optional.
    if (::fdatasync(fd_) != 0) {
      return PosixError("fdatasync " + filename_, errno);
    }
#endif
    return Status::OK();
  }

  Status Close() override {
    if (fd_ < 0) {
      return Status::OK();
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      return PosixError("close " + filename_, errno);
    }
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};

// ---------------------------------------------------------------------------
// SequentialFile
// ---------------------------------------------------------------------------

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string filename, int fd)
      : filename_(std::move(filename)), fd_(fd) {}

  ~PosixSequentialFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  Status Read(size_t n, std::string* result) override {
    result->clear();
    if (fd_ < 0) {
      return Status::IOError(filename_ + ": file already closed");
    }
    result->resize(n);
    size_t total = 0;
    while (total < n) {
      const ssize_t r = ::read(fd_, &(*result)[total], n - total);
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        result->clear();
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        break;  // EOF
      }
      total += static_cast<size_t>(r);
    }
    result->resize(total);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (fd_ < 0) {
      return Status::IOError(filename_ + ": file already closed");
    }
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) {
      return PosixError("skip " + filename_, errno);
    }
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};

// ---------------------------------------------------------------------------
// RandomAccessFile (pread)
// ---------------------------------------------------------------------------

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd)
      : filename_(std::move(filename)), fd_(fd) {}

  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  Status Read(uint64_t offset, size_t n, std::string* result) const override {
    result->clear();
    if (fd_ < 0) {
      return Status::IOError(filename_ + ": file already closed");
    }
    result->resize(n);
    size_t total = 0;
    while (total < n) {
      const ssize_t r = ::pread(fd_, &(*result)[total], n - total,
                                static_cast<off_t>(offset + total));
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        result->clear();
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        break;  // EOF
      }
      total += static_cast<size_t>(r);
    }
    result->resize(total);
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}

  ~PosixFileLock() override {
    // UnlockFile should release; destructor is a safety net for fd only.
    // Do not unlock here without going through Env (held-lock set).
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  const std::string& name() const { return name_; }
  int fd() const { return fd_; }

  int ReleaseFd() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  std::string name_;
  int fd_;
};

bool LockOrUnlock(int fd, bool lock) {
  struct flock f;
  std::memset(&f, 0, sizeof(f));
  f.l_type = lock ? F_WRLCK : F_UNLCK;
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;  // whole file
  return ::fcntl(fd, F_SETLK, &f) != -1;
}

// ---------------------------------------------------------------------------
// PosixEnv
// ---------------------------------------------------------------------------

class PosixEnv final : public Env {
 public:
  PosixEnv() = default;
  ~PosixEnv() override = default;

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    // O_TRUNC: create or replace (SST/WAL open for write).
    const int fd =
        ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(fname, errno);
    }
    result->reset(new PosixWritableFile(fname, fd));
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    // O_APPEND: MANIFEST after NewDB/Recover continues the same file.
    const int fd =
        ::open(fname.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(fname, errno);
    }
    result->reset(new PosixWritableFile(fname, fd));
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(fname, errno);
    }
    result->reset(new PosixSequentialFile(fname, fd));
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(fname, errno);
    }
    result->reset(new PosixRandomAccessFile(fname, fd));
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return ::access(fname.c_str(), F_OK) == 0;
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
      return PosixError(dir, errno);
    }
    struct dirent* entry;
    // readdir is fine for educational single-threaded listing in tests/Open.
    while ((entry = ::readdir(d)) != nullptr) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      result->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) {
      return PosixError("unlink " + fname, errno);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
      return PosixError("mkdir " + dirname, errno);
    }
    return Status::OK();
  }

  Status DeleteDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) {
      return PosixError("rmdir " + dirname, errno);
    }
    return Status::OK();
  }

  Status RenameFile(const std::string& src,
                    const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError("rename " + src + " -> " + target, errno);
    }
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat sbuf;
    if (::stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      return PosixError(fname, errno);
    }
    *size = static_cast<uint64_t>(sbuf.st_size);
    return Status::OK();
  }

  Status LockFile(const std::string& fname,
                  std::unique_ptr<FileLock>* lock) override {
    *lock = nullptr;
    const int fd =
        ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return PosixError(fname, errno);
    }

    {
      std::lock_guard<std::mutex> guard(mu_);
      if (locked_files_.count(fname) != 0) {
        ::close(fd);
        return Status::IOError("lock " + fname + ": already held by process");
      }
      if (!LockOrUnlock(fd, true)) {
        const int err = errno;
        ::close(fd);
        return PosixError("lock " + fname, err);
      }
      locked_files_.insert(fname);
    }

    lock->reset(new PosixFileLock(fname, fd));
    return Status::OK();
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    if (!lock) {
      return Status::InvalidArgument("UnlockFile: null lock");
    }
    auto* plock = dynamic_cast<PosixFileLock*>(lock.get());
    if (plock == nullptr) {
      return Status::InvalidArgument("UnlockFile: unknown lock type");
    }

    {
      std::lock_guard<std::mutex> guard(mu_);
      locked_files_.erase(plock->name());
    }

    if (!LockOrUnlock(plock->fd(), false)) {
      return PosixError("unlock " + plock->name(), errno);
    }

    const int fd = plock->ReleaseFd();
    lock.reset();
    if (fd >= 0 && ::close(fd) != 0) {
      return PosixError("close lock", errno);
    }
    return Status::OK();
  }

  Status FsyncDir(const std::string& dirname) override {
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError("open dir " + dirname, errno);
    }
    Status s = Status::OK();
    if (::fsync(fd) != 0) {
      s = PosixError("fsync dir " + dirname, errno);
    }
    ::close(fd);
    return s;
  }

  Status NewTempDir(std::string* path) override {
    // mkdtemp requires a writable template ending in six 'X's.
    std::string tmpl = "/tmp/tinylsm_test_XXXXXX";
    // mkdtemp modifies the buffer in place.
    if (::mkdtemp(&tmpl[0]) == nullptr) {
      return PosixError("mkdtemp", errno);
    }
    *path = tmpl;
    return Status::OK();
  }

  uint64_t NowMicros() override {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ull +
           static_cast<uint64_t>(tv.tv_usec);
  }

 private:
  // fcntl locks are per-process; track held paths so a second LockFile in the
  // same process fails (critical for single-process DB exclusivity tests).
  std::mutex mu_;
  std::set<std::string> locked_files_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Virtual destructors (out-of-line for vtable)
// ---------------------------------------------------------------------------

WritableFile::~WritableFile() = default;
SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
FileLock::~FileLock() = default;
Env::~Env() = default;

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace tinylsm
