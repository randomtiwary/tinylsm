// WAL writer/reader with framed CRC-32C records (docs/format.md §4).

#include "wal.h"

#include "tinylsm/coding.h"
#include "tinylsm/crc32c.h"

#include <string>
#include <utility>

namespace tinylsm {

void EncodeWalPayload(SequenceNumber seq, uint8_t type, std::string_view key,
                      std::string_view value, std::string* out) {
  out->clear();
  PutFixed64(out, seq);
  out->push_back(static_cast<char>(type));
  PutLengthPrefixedSlice(out, key);
  PutLengthPrefixedSlice(out, value);
}

Status DecodeWalPayload(std::string_view payload, SequenceNumber* seq,
                        uint8_t* type, std::string* key, std::string* value) {
  // Minimum: fixed64 + type + two zero-length varints (key_len, val_len).
  if (payload.size() < 8 + 1 + 1 + 1) {
    return Status::Corruption("WAL payload too short");
  }

  *seq = DecodeFixed64(payload.data());
  payload.remove_prefix(8);
  *type = static_cast<uint8_t>(payload[0]);
  payload.remove_prefix(1);

  std::string_view key_view;
  std::string_view value_view;
  if (!GetLengthPrefixedSlice(&payload, &key_view)) {
    return Status::Corruption("WAL payload bad key encoding");
  }
  if (!GetLengthPrefixedSlice(&payload, &value_view)) {
    return Status::Corruption("WAL payload bad value encoding");
  }
  if (!payload.empty()) {
    return Status::Corruption("WAL payload has trailing bytes");
  }

  key->assign(key_view.data(), key_view.size());
  value->assign(value_view.data(), value_view.size());
  return Status::OK();
}

// ---------------------------------------------------------------------------
// WalWriter
// ---------------------------------------------------------------------------

WalWriter::WalWriter(std::unique_ptr<WritableFile> file)
    : file_(std::move(file)) {}

WalWriter::~WalWriter() = default;

Status WalWriter::AddRecord(std::string_view payload) {
  if (file_ == nullptr) {
    return Status::IOError("WAL writer already closed");
  }
  // Frame: length | crc_masked | payload  (CRC over payload only, then mask).
  std::string frame;
  frame.reserve(8 + payload.size());
  PutFixed32(&frame, static_cast<uint32_t>(payload.size()));
  const uint32_t crc = crc32c::Value(payload.data(), payload.size());
  PutFixed32(&frame, crc32c::Mask(crc));
  frame.append(payload.data(), payload.size());
  return file_->Append(frame);
}

Status WalWriter::AddRecord(SequenceNumber seq, uint8_t type,
                            std::string_view key, std::string_view value) {
  std::string payload;
  EncodeWalPayload(seq, type, key, value, &payload);
  return AddRecord(std::string_view(payload));
}

Status WalWriter::Sync() {
  if (file_ == nullptr) {
    return Status::IOError("WAL writer already closed");
  }
  return file_->Sync();
}

Status WalWriter::Close() {
  if (file_ == nullptr) {
    return Status::OK();
  }
  Status s = file_->Close();
  file_.reset();
  return s;
}

// ---------------------------------------------------------------------------
// WalReader
// ---------------------------------------------------------------------------

WalReader::WalReader(std::unique_ptr<SequentialFile> file)
    : file_(std::move(file)) {}

WalReader::~WalReader() = default;

Status WalReader::ReadRecord(std::string* payload, bool* eof) {
  payload->clear();
  *eof = false;

  if (file_ == nullptr) {
    return Status::IOError("WAL reader has no file");
  }

  // 1. Need a full 8-byte header; fewer bytes remaining → clean stop (EOF/tear).
  std::string header;
  Status s = file_->Read(8, &header);
  if (!s.ok()) {
    return s;
  }
  if (header.size() < 8) {
    *eof = true;
    return Status::OK();
  }

  const uint32_t length = DecodeFixed32(header.data());
  const uint32_t crc_masked = DecodeFixed32(header.data() + 4);

  // 2. Need a full payload of `length` bytes; short read → torn tail (not Corruption).
  s = file_->Read(length, payload);
  if (!s.ok()) {
    payload->clear();
    return s;
  }
  if (payload->size() != length) {
    payload->clear();
    *eof = true;
    return Status::OK();
  }

  // 3. Full frame present: verify CRC of payload only. Mismatch → Corruption
  //    even if this is the last record at EOF (bitrot, not a tear).
  const uint32_t expected = crc32c::Unmask(crc_masked);
  const uint32_t actual = crc32c::Value(payload->data(), payload->size());
  if (actual != expected) {
    payload->clear();
    return Status::Corruption("WAL record CRC mismatch");
  }
  return Status::OK();
}

Status WalReader::ReadRecord(SequenceNumber* seq, uint8_t* type,
                             std::string* key, std::string* value, bool* eof) {
  std::string payload;
  Status s = ReadRecord(&payload, eof);
  if (!s.ok() || *eof) {
    return s;
  }
  return DecodeWalPayload(payload, seq, type, key, value);
}

}  // namespace tinylsm
