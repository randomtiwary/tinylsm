#pragma once

// DB directory file-name helpers (see docs/format.md §1 / design file naming).
// Patterns:
//   WAL:       {number}.log
//   SSTable:   {number}.sst
//   SST temp:  {number}.sst.tmp
//   Manifest:  MANIFEST-{number}
//   Current:   CURRENT
//   Lock:      LOCK
//   Generic temp: {number}.dbtmp
//
// Numbers are zero-padded to 6 digits for readability (not required for
// correctness; parsers accept any decimal width).

#include <cstdint>
#include <string>

namespace tinylsm {

// dbname/000001.log
std::string LogFileName(const std::string& dbname, uint64_t number);

// dbname/000001.sst
std::string TableFileName(const std::string& dbname, uint64_t number);

// dbname/000001.sst.tmp  (write path; rename to .sst when durable)
std::string TableTempFileName(const std::string& dbname, uint64_t number);

// dbname/MANIFEST-000001
std::string ManifestFileName(const std::string& dbname, uint64_t number);

// dbname/CURRENT
std::string CurrentFileName(const std::string& dbname);

// dbname/LOCK
std::string LockFileName(const std::string& dbname);

// dbname/000001.dbtmp  (generic temp, LevelDB-style)
std::string TempFileName(const std::string& dbname, uint64_t number);

}  // namespace tinylsm
