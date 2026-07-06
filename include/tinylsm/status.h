#pragma once

#include <string>

namespace tinylsm {

// Result type for operations. Prefer returning Status over exceptions for control flow.
// OK is cheap (no heap); error Statuses carry a code and optional message.
class Status {
 public:
  // Create a success status.
  Status() noexcept : code_(kOk) {}

  Status(const Status& s) = default;
  Status& operator=(const Status& s) = default;
  Status(Status&& s) noexcept = default;
  Status& operator=(Status&& s) noexcept = default;

  ~Status() = default;

  // Static constructors.
  static Status OK() { return Status(); }

  static Status NotFound(const std::string& msg) {
    return Status(kNotFound, msg);
  }
  static Status Corruption(const std::string& msg) {
    return Status(kCorruption, msg);
  }
  static Status IOError(const std::string& msg) {
    return Status(kIOError, msg);
  }
  static Status InvalidArgument(const std::string& msg) {
    return Status(kInvalidArgument, msg);
  }

  // Returns true iff the status indicates success.
  bool ok() const { return code_ == kOk; }

  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }

  // Human-readable form, e.g. "OK" or "NotFound: key missing".
  std::string ToString() const;

 private:
  enum Code : int {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4,
  };

  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_;
  std::string msg_;
};

}  // namespace tinylsm
