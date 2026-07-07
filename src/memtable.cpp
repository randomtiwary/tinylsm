#include "memtable.h"

#include "tinylsm/coding.h"

#include <cassert>
#include <cstring>

namespace tinylsm {
namespace {

// Encode a memtable entry into *dst:
//   varint32(internal_key.size()) || internal_key ||
//   varint32(value.size()) || value
void EncodeEntry(std::string* dst, std::string_view internal_key,
                 std::string_view value) {
  PutLengthPrefixedSlice(dst, internal_key);
  PutLengthPrefixedSlice(dst, value);
}

}  // namespace

int MemTable::KeyComparator::Compare(const char* a, const char* b) const {
  // Keys are length-prefixed internal keys (value payload follows; ignored).
  // Entry buffers are well-formed; only the internal-key prefix is compared.
  uint32_t a_len = 0;
  uint32_t b_len = 0;
  const char* a_key_start = GetVarint32Ptr(a, a + kMaxVarint32Length, &a_len);
  const char* b_key_start = GetVarint32Ptr(b, b + kMaxVarint32Length, &b_len);
  assert(a_key_start != nullptr);
  assert(b_key_start != nullptr);
  return comparator.Compare(std::string_view(a_key_start, a_len),
                            std::string_view(b_key_start, b_len));
}

std::string_view MemTable::GetLengthPrefixedInternalKey(const char* entry) {
  uint32_t len = 0;
  const char* key_start =
      GetVarint32Ptr(entry, entry + kMaxVarint32Length, &len);
  assert(key_start != nullptr);
  return std::string_view(key_start, len);
}

std::string_view MemTable::GetLengthPrefixedValue(const char* entry) {
  uint32_t key_len = 0;
  const char* key_start =
      GetVarint32Ptr(entry, entry + kMaxVarint32Length, &key_len);
  assert(key_start != nullptr);
  const char* value_tag = key_start + key_len;
  uint32_t value_len = 0;
  const char* value_start =
      GetVarint32Ptr(value_tag, value_tag + kMaxVarint32Length, &value_len);
  assert(value_start != nullptr);
  return std::string_view(value_start, value_len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), table_(comparator_) {}

MemTable::~MemTable() {
  for (char* p : arena_) {
    delete[] p;
  }
}

void MemTable::Add(SequenceNumber sequence, ValueType type,
                   std::string_view key, std::string_view value) {
  // Internal key: user_key || fixed64_le((seq << 8) | type)
  std::string internal_key;
  internal_key.reserve(key.size() + 8);
  AppendInternalKey(&internal_key, key, sequence, type);

  std::string encoded;
  encoded.reserve(key.size() + value.size() + 8 + 2 * kMaxVarint32Length);
  EncodeEntry(&encoded, internal_key, value);

  char* buf = new char[encoded.size()];
  std::memcpy(buf, encoded.data(), encoded.size());
  arena_.push_back(buf);

  // Approximate: entry bytes + skiplist node overhead (pointer + a few levels).
  approx_memory_usage_ += encoded.size() + sizeof(void*) * 8;

  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& lkey, std::string* value, Status* s) const {
  // Seek using a temporary entry encoding of the lookup internal key.
  // Comparator only inspects the length-prefixed internal key.
  std::string seek_key;
  EncodeEntry(&seek_key, lkey.internal_key(), std::string_view());

  Table::Iterator iter(&table_);
  iter.Seek(seek_key.data());
  if (!iter.Valid()) {
    *s = Status::NotFound(std::string());
    return false;  // absent — allow fall-through
  }

  const std::string_view found_ikey = GetLengthPrefixedInternalKey(iter.key());
  if (ExtractUserKey(found_ikey) != lkey.user_key()) {
    *s = Status::NotFound(std::string());
    return false;  // different user key — allow fall-through
  }

  // Found an entry for this user key: stop layered search either way.
  switch (ExtractValueType(found_ikey)) {
    case kTypeValue: {
      const std::string_view v = GetLengthPrefixedValue(iter.key());
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      // Tombstone: NotFound to caller, but true so we do not resurrect SST values.
      *s = Status::NotFound(std::string());
      return true;
  }
  *s = Status::NotFound(std::string());
  return false;
}

MemTable::Iterator::Iterator(const MemTable* table)
    : table_(table), iter_(&table->table_) {}

bool MemTable::Iterator::Valid() const { return iter_.Valid(); }

void MemTable::Iterator::SeekToFirst() { iter_.SeekToFirst(); }

void MemTable::Iterator::Seek(std::string_view internal_key) {
  seek_scratch_.clear();
  EncodeEntry(&seek_scratch_, internal_key, std::string_view());
  iter_.Seek(seek_scratch_.data());
}

void MemTable::Iterator::Next() { iter_.Next(); }

std::string_view MemTable::Iterator::key() const {
  assert(Valid());
  return MemTable::GetLengthPrefixedInternalKey(iter_.key());
}

std::string_view MemTable::Iterator::value() const {
  assert(Valid());
  return MemTable::GetLengthPrefixedValue(iter_.key());
}

}  // namespace tinylsm
