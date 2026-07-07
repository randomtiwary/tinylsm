#pragma once

// Write-ahead log: framed CRC records (docs/format.md §4).
// Physical frame: fixed32_le length | fixed32_le crc32c_masked | payload[length]
// Payload: fixed64_le sequence | u8 value_type | varint32 key_len | key |
//          varint32 val_len | value
//
// Internal component (not part of the public installed API surface).

#include "internal_key.h"

#include "tinylsm/env.h"
#include "tinylsm/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tinylsm {

// ValueType / SequenceNumber / kType* come from internal_key.h (docs/format.md §3 / §4.2).

// Encode a logical Put/Delete into a WAL payload (no physical frame).
void EncodeWalPayload(SequenceNumber seq, uint8_t type, std::string_view key,
                      std::string_view value, std::string* out);

// Decode a WAL payload produced by EncodeWalPayload.
// Returns Corruption if the payload is malformed.
Status DecodeWalPayload(std::string_view payload, SequenceNumber* seq,
                        uint8_t* type, std::string* key, std::string* value);

// Append-only WAL writer over a WritableFile.
class WalWriter {
 public:
  explicit WalWriter(std::unique_ptr<WritableFile> file);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Append one framed record for a pre-encoded payload.
  Status AddRecord(std::string_view payload);

  // Encode and append one logical record (Put or Delete).
  Status AddRecord(SequenceNumber seq, uint8_t type, std::string_view key,
                   std::string_view value);

  // fdatasync/fsync the underlying file (WriteOptions.sync path).
  Status Sync();

  Status Close();

 private:
  std::unique_ptr<WritableFile> file_;
};

// Sequential WAL reader for recovery scans (docs/format.md §4.4).
// Torn tail / incomplete final record → stop successfully (not Corruption).
// Full framed record with bad CRC → Corruption.
class WalReader {
 public:
  explicit WalReader(std::unique_ptr<SequentialFile> file);
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  // Read the next complete physical record's payload into *payload.
  // On clean EOF or torn tail: returns OK and sets *eof = true (payload empty).
  // On success with a record: returns OK, *eof = false, *payload filled.
  // On full-frame CRC mismatch: Corruption.
  Status ReadRecord(std::string* payload, bool* eof);

  // Read and decode the next logical record.
  // Same EOF/torn-tail/Corruption rules as the payload overload.
  Status ReadRecord(SequenceNumber* seq, uint8_t* type, std::string* key,
                    std::string* value, bool* eof);

 private:
  std::unique_ptr<SequentialFile> file_;
};

}  // namespace tinylsm
