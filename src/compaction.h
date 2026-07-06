#pragma once

// L0→L1 leveled compaction: input selection, merge, bottommost tombstone drop.
// See design doc §6. Educational v1: two levels only; single L1 output file.

#include "version_edit.h"
#include "version_set.h"

#include "tinylsm/env.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

// Inclusive user-key range overlap (bytewise).
bool UserKeyRangesOverlap(std::string_view a_smallest, std::string_view a_largest,
                          std::string_view b_smallest, std::string_view b_largest);

// Compaction input set for one L0→L1 job.
struct CompactionInputs {
  // All L0 files (normative when triggered).
  std::vector<std::shared_ptr<FileMetaData>> level0;
  // Every L1 file whose user-key range overlaps the L0 union range.
  std::vector<std::shared_ptr<FileMetaData>> level1;
  // Output level is always L1; with only two levels, L1 is bottommost.
  bool bottommost = true;

  bool empty() const { return level0.empty(); }

  // Union user-key range over level0 (undefined if level0 empty).
  void L0UserKeyRange(std::string* smallest_uk, std::string* largest_uk) const;
};

// True when L0 file count >= l0_compaction_trigger.
bool NeedsCompaction(const Version& version, int l0_compaction_trigger);

// Pick all L0 + all overlapping L1. Returns empty().level0 if not needed.
CompactionInputs PickCompaction(const Version& version, int l0_compaction_trigger);

// Open inputs, merge by InternalKeyComparator (newest-first per user key),
// emit at most one entry per user key; drop deletions when bottommost.
// Writes a single L1 SST at output->number (pre-assigned). On success fills
// file_size/smallest/largest. If the merge emits zero keys, *wrote_output is
// false and no durable SST is left (temps cleaned up).
//
// Mutex must NOT be held (SST IO).
Status DoCompactionWork(Env* env, const std::string& dbname,
                        const Options& options, const CompactionInputs& inputs,
                        FileMetaData* output, bool* wrote_output);

}  // namespace tinylsm
