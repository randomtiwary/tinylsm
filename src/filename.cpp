#include "tinylsm/filename.h"

#include <cstdio>

namespace tinylsm {
namespace {

// Join dbname and a relative name with a single '/'.
std::string MakeFileName(const std::string& dbname, const std::string& name) {
  std::string result = dbname;
  if (!result.empty() && result.back() != '/') {
    result.push_back('/');
  }
  result.append(name);
  return result;
}

// Zero-pad number to 6 digits (educational readability; parsers accept any width).
std::string FormatNumber(uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu",
                static_cast<unsigned long long>(number));
  return std::string(buf);
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, FormatNumber(number) + ".log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, FormatNumber(number) + ".sst");
}

std::string TableTempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, FormatNumber(number) + ".sst.tmp");
}

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, "MANIFEST-" + FormatNumber(number));
}

std::string CurrentFileName(const std::string& dbname) {
  return MakeFileName(dbname, "CURRENT");
}

std::string LockFileName(const std::string& dbname) {
  return MakeFileName(dbname, "LOCK");
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, FormatNumber(number) + ".dbtmp");
}

}  // namespace tinylsm
