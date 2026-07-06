#include "internal_key.h"

#include <cstring>

namespace tinylsm {

std::string EncodeInternalKey(std::string_view user_key, SequenceNumber seq,
                              ValueType type) {
  std::string result;
  result.reserve(user_key.size() + 8);
  AppendInternalKey(&result, user_key, seq, type);
  return result;
}

void AppendInternalKey(std::string* dst, std::string_view user_key,
                       SequenceNumber seq, ValueType type) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackSequenceAndType(seq, type));
}

bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < 8) {
    return false;
  }
  const size_t ukey_size = internal_key.size() - 8;
  result->user_key = std::string_view(internal_key.data(), ukey_size);
  const uint64_t packed = DecodeFixed64(internal_key.data() + ukey_size);
  result->sequence = packed >> 8;
  result->type = static_cast<ValueType>(packed & 0xff);
  return true;
}

int InternalKeyComparator::Compare(std::string_view a,
                                   std::string_view b) const {
  // Compare user keys bytewise ascending.
  const std::string_view a_user = ExtractUserKey(a);
  const std::string_view b_user = ExtractUserKey(b);
  const int r = a_user.compare(b_user);
  if (r != 0) {
    return r;
  }

  // Equal user keys: higher sequence first (descending).
  // For malformed short keys ExtractSequence returns 0; still define an order.
  const SequenceNumber a_seq = ExtractSequence(a);
  const SequenceNumber b_seq = ExtractSequence(b);
  if (a_seq > b_seq) {
    return -1;
  }
  if (a_seq < b_seq) {
    return 1;
  }

  // Equal sequence: higher type first (descending as unsigned 8-bit).
  const uint8_t a_type = static_cast<uint8_t>(ExtractValueType(a));
  const uint8_t b_type = static_cast<uint8_t>(ExtractValueType(b));
  if (a_type > b_type) {
    return -1;
  }
  if (a_type < b_type) {
    return 1;
  }
  return 0;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  space_.reserve(user_key.size() + 8);
  AppendInternalKey(&space_, user_key, sequence, kTypeValue);
  internal_key_ = std::string_view(space_.data(), space_.size());
}

}  // namespace tinylsm
