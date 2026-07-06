#include "tinylsm/status.h"

namespace tinylsm {

std::string Status::ToString() const {
  if (code_ == kOk) {
    return "OK";
  }
  const char* type = nullptr;
  switch (code_) {
    case kOk:
      type = "OK";
      break;
    case kNotFound:
      type = "NotFound";
      break;
    case kCorruption:
      type = "Corruption";
      break;
    case kIOError:
      type = "IOError";
      break;
    case kInvalidArgument:
      type = "InvalidArgument";
      break;
  }
  std::string result(type);
  if (!msg_.empty()) {
    result.append(": ");
    result.append(msg_);
  }
  return result;
}

}  // namespace tinylsm
