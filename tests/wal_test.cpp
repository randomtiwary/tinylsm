#include "wal.h"

#include "tinylsm/coding.h"
#include "tinylsm/crc32c.h"
#include "tinylsm/env.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = tinylsm::Env::Default();
    ASSERT_NE(env_, nullptr);
    ASSERT_TRUE(env_->NewTempDir(&tmpdir_).ok());
    ASSERT_FALSE(tmpdir_.empty());
    path_ = tmpdir_ + "/000001.log";
  }

  void TearDown() override {
    if (tmpdir_.empty()) {
      return;
    }
    std::vector<std::string> children;
    if (env_->GetChildren(tmpdir_, &children).ok()) {
      for (const auto& name : children) {
        (void)env_->DeleteFile(tmpdir_ + "/" + name);
      }
    }
    (void)env_->DeleteDir(tmpdir_);
  }

  // Open a writer on path_ (create/truncate).
  std::unique_ptr<tinylsm::WalWriter> OpenWriter() {
    std::unique_ptr<tinylsm::WritableFile> file;
    EXPECT_TRUE(env_->NewWritableFile(path_, &file).ok());
    return std::make_unique<tinylsm::WalWriter>(std::move(file));
  }

  // Open a reader on path_.
  std::unique_ptr<tinylsm::WalReader> OpenReader() {
    std::unique_ptr<tinylsm::SequentialFile> file;
    EXPECT_TRUE(env_->NewSequentialFile(path_, &file).ok());
    return std::make_unique<tinylsm::WalReader>(std::move(file));
  }

  // Read entire path_ into *out.
  void ReadWholeFile(std::string* out) {
    std::unique_ptr<tinylsm::SequentialFile> file;
    ASSERT_TRUE(env_->NewSequentialFile(path_, &file).ok());
    out->clear();
    for (;;) {
      std::string chunk;
      ASSERT_TRUE(file->Read(4096, &chunk).ok());
      if (chunk.empty()) {
        break;
      }
      out->append(chunk);
    }
  }

  // Rewrite path_ with exact bytes.
  void WriteWholeFile(const std::string& data) {
    std::unique_ptr<tinylsm::WritableFile> file;
    ASSERT_TRUE(env_->NewWritableFile(path_, &file).ok());
    ASSERT_TRUE(file->Append(data).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  struct Record {
    tinylsm::SequenceNumber seq;
    uint8_t type;
    std::string key;
    std::string value;
  };

  // Drain reader until eof; push records into *out. Stops on non-OK status.
  tinylsm::Status Drain(tinylsm::WalReader* reader,
                        std::vector<Record>* out) {
    out->clear();
    for (;;) {
      Record r;
      bool eof = false;
      tinylsm::Status s =
          reader->ReadRecord(&r.seq, &r.type, &r.key, &r.value, &eof);
      if (!s.ok()) {
        return s;
      }
      if (eof) {
        return tinylsm::Status::OK();
      }
      out->push_back(std::move(r));
    }
  }

  tinylsm::Env* env_ = nullptr;
  std::string tmpdir_;
  std::string path_;
};

TEST_F(WalTest, EmptyFile) {
  // Create empty log file.
  {
    std::unique_ptr<tinylsm::WritableFile> file;
    ASSERT_TRUE(env_->NewWritableFile(path_, &file).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  auto reader = OpenReader();
  ASSERT_NE(reader, nullptr);

  std::string payload;
  bool eof = false;
  ASSERT_TRUE(reader->ReadRecord(&payload, &eof).ok());
  EXPECT_TRUE(eof);
  EXPECT_TRUE(payload.empty());

  // Second call also EOF.
  eof = false;
  ASSERT_TRUE(reader->ReadRecord(&payload, &eof).ok());
  EXPECT_TRUE(eof);
}

TEST_F(WalTest, RoundTripMultiRecord) {
  {
    auto writer = OpenWriter();
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        writer->AddRecord(1, tinylsm::kTypeValue, "alpha", "one").ok());
    ASSERT_TRUE(
        writer->AddRecord(2, tinylsm::kTypeValue, "beta", "two").ok());
    ASSERT_TRUE(
        writer->AddRecord(3, tinylsm::kTypeDeletion, "alpha", "").ok());
    ASSERT_TRUE(
        writer->AddRecord(4, tinylsm::kTypeValue, "gamma", std::string(256, 'x'))
            .ok());
    ASSERT_TRUE(writer->Sync().ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  auto reader = OpenReader();
  ASSERT_NE(reader, nullptr);
  std::vector<Record> records;
  ASSERT_TRUE(Drain(reader.get(), &records).ok());
  ASSERT_EQ(records.size(), 4u);

  EXPECT_EQ(records[0].seq, 1u);
  EXPECT_EQ(records[0].type, tinylsm::kTypeValue);
  EXPECT_EQ(records[0].key, "alpha");
  EXPECT_EQ(records[0].value, "one");

  EXPECT_EQ(records[1].seq, 2u);
  EXPECT_EQ(records[1].type, tinylsm::kTypeValue);
  EXPECT_EQ(records[1].key, "beta");
  EXPECT_EQ(records[1].value, "two");

  EXPECT_EQ(records[2].seq, 3u);
  EXPECT_EQ(records[2].type, tinylsm::kTypeDeletion);
  EXPECT_EQ(records[2].key, "alpha");
  EXPECT_EQ(records[2].value, "");

  EXPECT_EQ(records[3].seq, 4u);
  EXPECT_EQ(records[3].type, tinylsm::kTypeValue);
  EXPECT_EQ(records[3].key, "gamma");
  EXPECT_EQ(records[3].value, std::string(256, 'x'));
}

TEST_F(WalTest, PayloadEncodeDecodeRoundTrip) {
  std::string payload;
  tinylsm::EncodeWalPayload(42, tinylsm::kTypeValue, "k", "v", &payload);

  tinylsm::SequenceNumber seq = 0;
  uint8_t type = 0xff;
  std::string key, value;
  ASSERT_TRUE(
      tinylsm::DecodeWalPayload(payload, &seq, &type, &key, &value).ok());
  EXPECT_EQ(seq, 42u);
  EXPECT_EQ(type, tinylsm::kTypeValue);
  EXPECT_EQ(key, "k");
  EXPECT_EQ(value, "v");
}

TEST_F(WalTest, TornTailIncompletePayload) {
  // Two complete records, then a partial third (header OK, payload truncated).
  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(1, tinylsm::kTypeValue, "a", "1").ok());
    ASSERT_TRUE(writer->AddRecord(2, tinylsm::kTypeValue, "b", "2").ok());
    ASSERT_TRUE(writer->AddRecord(3, tinylsm::kTypeValue, "c", "three").ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  std::string full;
  ReadWholeFile(&full);
  ASSERT_GT(full.size(), 8u);

  // Drop the last 3 bytes so the final payload is incomplete.
  const std::string truncated = full.substr(0, full.size() - 3);
  WriteWholeFile(truncated);

  auto reader = OpenReader();
  std::vector<Record> records;
  // Torn tail must stop successfully, keeping the first two records.
  ASSERT_TRUE(Drain(reader.get(), &records).ok())
      << "torn tail must not be Corruption";
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].key, "a");
  EXPECT_EQ(records[1].key, "b");
}

TEST_F(WalTest, TornTailIncompleteHeader) {
  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(1, tinylsm::kTypeValue, "a", "1").ok());
    ASSERT_TRUE(writer->AddRecord(2, tinylsm::kTypeValue, "b", "2").ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  std::string full;
  ReadWholeFile(&full);

  // Append a partial header (only 3 of 8 bytes) after two good records.
  full.push_back('\x01');
  full.push_back('\x02');
  full.push_back('\x03');
  WriteWholeFile(full);

  auto reader = OpenReader();
  std::vector<Record> records;
  ASSERT_TRUE(Drain(reader.get(), &records).ok());
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].key, "a");
  EXPECT_EQ(records[1].key, "b");
}

TEST_F(WalTest, TornTailOnlyPartialHeader) {
  // File contains only a few header bytes — empty effective log.
  WriteWholeFile(std::string("\x01\x00\x00", 3));

  auto reader = OpenReader();
  std::string payload;
  bool eof = false;
  ASSERT_TRUE(reader->ReadRecord(&payload, &eof).ok());
  EXPECT_TRUE(eof);
  EXPECT_TRUE(payload.empty());
}

TEST_F(WalTest, MidStreamCorruptionBadCrc) {
  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(1, tinylsm::kTypeValue, "good1", "v1").ok());
    ASSERT_TRUE(writer->AddRecord(2, tinylsm::kTypeValue, "bad", "v2").ok());
    ASSERT_TRUE(writer->AddRecord(3, tinylsm::kTypeValue, "good2", "v3").ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  std::string full;
  ReadWholeFile(&full);

  // Locate second record: read first frame length to find offset of second.
  ASSERT_GE(full.size(), 8u);
  const uint32_t len0 = tinylsm::DecodeFixed32(full.data());
  const size_t rec1_off = 8 + len0;
  ASSERT_GT(full.size(), rec1_off + 8u);

  // Flip a bit in the second record's CRC field (bytes [rec1_off+4 .. +8)).
  full[rec1_off + 4] = static_cast<char>(
      static_cast<unsigned char>(full[rec1_off + 4]) ^ 0xffu);
  WriteWholeFile(full);

  auto reader = OpenReader();

  // First record OK.
  tinylsm::SequenceNumber seq = 0;
  uint8_t type = 0;
  std::string key, value;
  bool eof = false;
  ASSERT_TRUE(reader->ReadRecord(&seq, &type, &key, &value, &eof).ok());
  EXPECT_FALSE(eof);
  EXPECT_EQ(key, "good1");

  // Second record: full frame with bad CRC → Corruption (not torn tail).
  tinylsm::Status s = reader->ReadRecord(&seq, &type, &key, &value, &eof);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_FALSE(eof);
}

TEST_F(WalTest, MidStreamCorruptionFlipPayloadByte) {
  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(10, tinylsm::kTypeValue, "k", "payload").ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  std::string full;
  ReadWholeFile(&full);
  ASSERT_GE(full.size(), 9u);
  // Flip a byte inside the payload region (after 8-byte header).
  full[8] = static_cast<char>(static_cast<unsigned char>(full[8]) ^ 0x01u);
  WriteWholeFile(full);

  auto reader = OpenReader();
  std::string payload;
  bool eof = false;
  tinylsm::Status s = reader->ReadRecord(&payload, &eof);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_FALSE(eof);
}

TEST_F(WalTest, PhysicalFrameLayout) {
  // Hand-check one framed record's length and masked CRC match the payload.
  std::string payload;
  tinylsm::EncodeWalPayload(7, tinylsm::kTypeValue, "key", "val", &payload);

  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(std::string_view(payload)).ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  std::string full;
  ReadWholeFile(&full);
  ASSERT_EQ(full.size(), 8u + payload.size());
  EXPECT_EQ(tinylsm::DecodeFixed32(full.data()),
            static_cast<uint32_t>(payload.size()));
  const uint32_t stored_crc = tinylsm::DecodeFixed32(full.data() + 4);
  const uint32_t raw =
      tinylsm::crc32c::Value(payload.data(), payload.size());
  EXPECT_EQ(stored_crc, tinylsm::crc32c::Mask(raw));
  EXPECT_EQ(tinylsm::crc32c::Unmask(stored_crc), raw);
  EXPECT_EQ(full.substr(8), payload);
}

TEST_F(WalTest, AddRecordRawPayload) {
  std::string payload;
  tinylsm::EncodeWalPayload(99, tinylsm::kTypeDeletion, "gone", "", &payload);

  {
    auto writer = OpenWriter();
    ASSERT_TRUE(writer->AddRecord(std::string_view(payload)).ok());
    ASSERT_TRUE(writer->Sync().ok());
    ASSERT_TRUE(writer->Close().ok());
  }

  auto reader = OpenReader();
  tinylsm::SequenceNumber seq = 0;
  uint8_t type = 0;
  std::string key, value;
  bool eof = false;
  ASSERT_TRUE(reader->ReadRecord(&seq, &type, &key, &value, &eof).ok());
  EXPECT_FALSE(eof);
  EXPECT_EQ(seq, 99u);
  EXPECT_EQ(type, tinylsm::kTypeDeletion);
  EXPECT_EQ(key, "gone");
  EXPECT_EQ(value, "");

  ASSERT_TRUE(reader->ReadRecord(&seq, &type, &key, &value, &eof).ok());
  EXPECT_TRUE(eof);
}

}  // namespace
