#include "compaction.h"

#include "internal_key.h"
#include "table.h"
#include "table_builder.h"
#include "tinylsm/filename.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace tinylsm {
namespace {

struct OpenTable {
  std::shared_ptr<FileMetaData> meta;
  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
  std::unique_ptr<Table::Iterator> iter;
};

// Min-heap entry: smallest internal key first (InternalKeyComparator order).
struct HeapItem {
  std::string_view key;
  size_t index;  // into OpenTable vector
};

struct HeapCmp {
  const InternalKeyComparator* icmp;
  bool operator()(const HeapItem& a, const HeapItem& b) const {
    // priority_queue is max-heap; invert so smallest key is top.
    return icmp->Compare(a.key, b.key) > 0;
  }
};

Status OpenInput(Env* env, const std::string& dbname,
                 const std::shared_ptr<FileMetaData>& meta, OpenTable* out) {
  out->meta = meta;
  const std::string path = TableFileName(dbname, meta->number);
  Status s = env->NewRandomAccessFile(path, &out->file);
  if (!s.ok()) {
    return s;
  }
  uint64_t file_size = meta->file_size;
  if (file_size == 0) {
    s = env->GetFileSize(path, &file_size);
    if (!s.ok()) {
      return s;
    }
  }
  s = Table::Open(out->file.get(), file_size, &out->table);
  if (!s.ok()) {
    return s;
  }
  out->iter = std::make_unique<Table::Iterator>(out->table->NewIterator());
  out->iter->SeekToFirst();
  if (!out->iter->status().ok()) {
    return out->iter->status();
  }
  return Status::OK();
}

}  // namespace

bool UserKeyRangesOverlap(std::string_view a_smallest,
                          std::string_view a_largest,
                          std::string_view b_smallest,
                          std::string_view b_largest) {
  // Inclusive: overlap unless a is entirely left of b or entirely right.
  if (a_largest < b_smallest) {
    return false;
  }
  if (b_largest < a_smallest) {
    return false;
  }
  return true;
}

void CompactionInputs::L0UserKeyRange(std::string* smallest_uk,
                                      std::string* largest_uk) const {
  assert(!level0.empty());
  assert(smallest_uk != nullptr && largest_uk != nullptr);
  *smallest_uk = std::string(ExtractUserKey(level0[0]->smallest));
  *largest_uk = std::string(ExtractUserKey(level0[0]->largest));
  for (size_t i = 1; i < level0.size(); ++i) {
    const std::string_view s = ExtractUserKey(level0[i]->smallest);
    const std::string_view l = ExtractUserKey(level0[i]->largest);
    if (s < *smallest_uk) {
      smallest_uk->assign(s.data(), s.size());
    }
    if (l > *largest_uk) {
      largest_uk->assign(l.data(), l.size());
    }
  }
}

bool NeedsCompaction(const Version& version, int l0_compaction_trigger) {
  if (l0_compaction_trigger <= 0) {
    return false;
  }
  return static_cast<int>(version.NumFiles(0)) >= l0_compaction_trigger;
}

CompactionInputs PickCompaction(const Version& version,
                                int l0_compaction_trigger) {
  CompactionInputs c;
  if (!NeedsCompaction(version, l0_compaction_trigger)) {
    return c;
  }
  // 1) All L0 files.
  const auto& l0 = version.LevelFiles(0);
  c.level0.assign(l0.begin(), l0.end());
  if (c.level0.empty()) {
    return c;
  }

  // 2) Union user-key range over L0.
  std::string range_small;
  std::string range_large;
  c.L0UserKeyRange(&range_small, &range_large);

  // 3) Every L1 file whose range overlaps that union.
  const auto& l1 = version.LevelFiles(1);
  for (const auto& f : l1) {
    const std::string_view fs = ExtractUserKey(f->smallest);
    const std::string_view fl = ExtractUserKey(f->largest);
    if (UserKeyRangesOverlap(range_small, range_large, fs, fl)) {
      c.level1.push_back(f);
    }
  }
  c.bottommost = true;  // L1 is the only lower level in v1
  return c;
}

Status DoCompactionWork(Env* env, const std::string& dbname,
                        const Options& options, const CompactionInputs& inputs,
                        FileMetaData* output, bool* wrote_output) {
  assert(env != nullptr);
  assert(output != nullptr);
  assert(wrote_output != nullptr);
  *wrote_output = false;

  if (inputs.level0.empty()) {
    return Status::InvalidArgument("DoCompactionWork: no L0 inputs");
  }

  std::vector<OpenTable> opens;
  opens.reserve(inputs.level0.size() + inputs.level1.size());

  auto open_all = [&](const std::vector<std::shared_ptr<FileMetaData>>& files)
      -> Status {
    for (const auto& meta : files) {
      OpenTable ot;
      Status s = OpenInput(env, dbname, meta, &ot);
      if (!s.ok()) {
        return s;
      }
      opens.push_back(std::move(ot));
    }
    return Status::OK();
  };

  Status s = open_all(inputs.level0);
  if (!s.ok()) {
    return s;
  }
  s = open_all(inputs.level1);
  if (!s.ok()) {
    return s;
  }

  InternalKeyComparator icmp;
  using PQ = std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCmp>;
  HeapCmp hcmp{&icmp};
  PQ heap(hcmp);

  for (size_t i = 0; i < opens.size(); ++i) {
    if (opens[i].iter->Valid()) {
      heap.push(HeapItem{opens[i].iter->key(), i});
    }
  }

  const std::string tmp = TableTempFileName(dbname, output->number);
  const std::string final_path = TableFileName(dbname, output->number);

  std::unique_ptr<WritableFile> file;
  s = env->NewWritableFile(tmp, &file);
  if (!s.ok()) {
    return s;
  }

  TableBuilder builder(file.get(), options.block_size,
                       options.bloom_bits_per_key);
  std::string last_user_key;
  bool has_last_user = false;
  // Own copies of key/value when emitting (heap views invalidated by Next).
  std::string emit_key;
  std::string emit_value;

  while (!heap.empty()) {
    HeapItem top = heap.top();
    heap.pop();
    OpenTable& src = opens[top.index];

    // Copy current entry before advancing (views into block storage).
    emit_key.assign(src.iter->key().data(), src.iter->key().size());
    emit_value.assign(src.iter->value().data(), src.iter->value().size());

    src.iter->Next();
    if (!src.iter->status().ok()) {
      (void)file->Close();
      (void)env->DeleteFile(tmp);
      return src.iter->status();
    }
    if (src.iter->Valid()) {
      heap.push(HeapItem{src.iter->key(), top.index});
    }

    ParsedInternalKey parsed;
    if (!ParseInternalKey(emit_key, &parsed)) {
      (void)file->Close();
      (void)env->DeleteFile(tmp);
      return Status::Corruption("compaction: bad internal key");
    }

    // At most one emit per user_key: first seen is newest (comparator order).
    if (has_last_user && parsed.user_key == last_user_key) {
      continue;  // superseded older version
    }
    last_user_key.assign(parsed.user_key.data(), parsed.user_key.size());
    has_last_user = true;

    // Bottommost tombstone drop: no lower level can hold older data.
    if (inputs.bottommost && parsed.type == kTypeDeletion) {
      continue;
    }

    builder.Add(emit_key, emit_value);
  }

  // Zero entries after drop: no output SST (just delete inputs in edit).
  if (builder.NumEntries() == 0) {
    builder.Abandon();
    (void)file->Close();
    (void)env->DeleteFile(tmp);
    *wrote_output = false;
    output->file_size = 0;
    output->smallest.clear();
    output->largest.clear();
    return Status::OK();
  }

  TableBuildStats stats;
  s = builder.Finish(&stats);
  if (!s.ok()) {
    (void)file->Close();
    (void)env->DeleteFile(tmp);
    return s;
  }
  s = file->Sync();
  if (!s.ok()) {
    (void)file->Close();
    (void)env->DeleteFile(tmp);
    return s;
  }
  s = file->Close();
  if (!s.ok()) {
    (void)env->DeleteFile(tmp);
    return s;
  }
  s = env->RenameFile(tmp, final_path);
  if (!s.ok()) {
    (void)env->DeleteFile(tmp);
    return s;
  }
  (void)env->FsyncDir(dbname);

  output->file_size = stats.file_size;
  output->smallest = stats.smallest_key;
  output->largest = stats.largest_key;
  *wrote_output = true;
  return Status::OK();
}

}  // namespace tinylsm
