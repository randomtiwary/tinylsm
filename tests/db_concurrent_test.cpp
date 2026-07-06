// Concurrent Put/Get/Delete stress and flush/compaction race tests (design C1–C5).
// Locking is assumed correct; these only stress the published model.
//
// Duration / thread count (CI-friendly defaults: ~6 threads, ~8s):
//   TINYLSM_STRESS_THREADS  (default 6, clamped 2..32)
//   TINYLSM_STRESS_SECONDS  (default 8,  clamped 1..120)

#include "db_impl.h"

#include "tinylsm/db.h"
#include "tinylsm/env.h"
#include "tinylsm/options.h"
#include "tinylsm/status.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDefaultStressThreads = 6;
constexpr int kDefaultStressSeconds = 8;
constexpr int kJoinGraceSeconds = 30;  // hard fail if workers hang past this

int StressThreads() {
  int n = kDefaultStressThreads;
  if (const char* e = std::getenv("TINYLSM_STRESS_THREADS")) {
    const int v = std::atoi(e);
    if (v > 0) {
      n = v;
    }
  }
  if (n < 2) {
    n = 2;
  }
  if (n > 32) {
    n = 32;
  }
  return n;
}

int StressSeconds() {
  int n = kDefaultStressSeconds;
  if (const char* e = std::getenv("TINYLSM_STRESS_SECONDS")) {
    const int v = std::atoi(e);
    if (v > 0) {
      n = v;
    }
  }
  if (n < 1) {
    n = 1;
  }
  if (n > 120) {
    n = 120;
  }
  return n;
}

// Join all worker threads. Returns false if join does not complete before
// hard_deadline (write-stall deadlock / hang). Detaches the joiner on timeout.
// Callers must ASSERT_TRUE the result so the test aborts on hang.
bool JoinAllWithDeadline(std::vector<std::thread>& threads,
                         std::chrono::steady_clock::time_point hard_deadline) {
  std::atomic<bool> all_joined{false};
  std::thread joiner([&]() {
    for (auto& t : threads) {
      if (t.joinable()) {
        t.join();
      }
    }
    all_joined.store(true, std::memory_order_release);
  });
  while (!all_joined.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < hard_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!all_joined.load(std::memory_order_acquire)) {
    joiner.detach();
    return false;
  }
  joiner.join();
  return true;
}

class DbConcurrentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = tinylsm::Env::Default();
    ASSERT_TRUE(env_->NewTempDir(&dbname_).ok());
  }

  void TearDown() override {
    if (!dbname_.empty()) {
      std::vector<std::string> children;
      if (env_->GetChildren(dbname_, &children).ok()) {
        for (const auto& name : children) {
          (void)env_->DeleteFile(dbname_ + "/" + name);
        }
      }
      (void)env_->DeleteDir(dbname_);
    }
  }

  tinylsm::Options SmallBufferOptions(size_t write_buffer = 512,
                                      int l0_trigger = 4) const {
    tinylsm::Options opt;
    opt.create_if_missing = true;
    opt.error_if_exists = false;
    opt.env = env_;
    opt.write_buffer_size = write_buffer;
    opt.l0_compaction_trigger = l0_trigger;
    opt.sync_writes = false;
    return opt;
  }

  static tinylsm::DBImpl* AsImpl(tinylsm::DB* db) {
    return static_cast<tinylsm::DBImpl*>(db);
  }

  tinylsm::Env* env_ = nullptr;
  std::string dbname_;
};

// ---------------------------------------------------------------------------
// C1: Concurrent Get during flush publish — many readers while writers force
// flushes via a small write_buffer_size. No crash; seeded keys remain valid.
// ---------------------------------------------------------------------------
TEST_F(DbConcurrentTest, C1_ConcurrentGetDuringFlushPublish) {
  const int n_threads = StressThreads();
  const int seconds = StressSeconds();
  const int n_readers = std::max(2, n_threads / 2);
  const int n_writers = std::max(2, n_threads - n_readers);

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(
      tinylsm::DB::Open(SmallBufferOptions(/*write_buffer=*/400), dbname_, &db)
          .ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  tinylsm::WriteOptions wo;
  wo.sync = false;
  constexpr int kSeedKeys = 64;
  const std::string payload(48, 'p');
  for (int i = 0; i < kSeedKeys; ++i) {
    ASSERT_TRUE(db->Put(wo, "seed" + std::to_string(i), payload).ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> writes{0};
  std::atomic<bool> reader_error{false};
  std::atomic<bool> writer_error{false};
  std::string reader_err_msg;
  std::string writer_err_msg;
  std::mutex err_mu;

  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(n_readers + n_writers));

  for (int r = 0; r < n_readers; ++r) {
    threads.emplace_back([&, r]() {
      tinylsm::ReadOptions local_ro;
      std::string value;
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string key = "seed" + std::to_string(local % kSeedKeys);
        tinylsm::Status s = db->Get(local_ro, key, &value);
        if (!s.ok() && !s.IsNotFound()) {
          std::lock_guard<std::mutex> g(err_mu);
          reader_err_msg = s.ToString();
          reader_error.store(true);
          return;
        }
        // Seed keys were written successfully; must remain present across
        // flush publish (mem → imm → L0).
        if (s.IsNotFound()) {
          std::lock_guard<std::mutex> g(err_mu);
          reader_err_msg = "seed key missing: " + key;
          reader_error.store(true);
          return;
        }
        if (value != payload) {
          std::lock_guard<std::mutex> g(err_mu);
          reader_err_msg = "unexpected value for " + key;
          reader_error.store(true);
          return;
        }
        // Writer keys may or may not exist yet; only reject hard errors / empty.
        const std::string wkey =
            "w" + std::to_string(r) + "_" + std::to_string(local % 32);
        s = db->Get(local_ro, wkey, &value);
        if (!s.ok() && !s.IsNotFound()) {
          std::lock_guard<std::mutex> g(err_mu);
          reader_err_msg = s.ToString();
          reader_error.store(true);
          return;
        }
        if (s.ok() && value.empty()) {
          std::lock_guard<std::mutex> g(err_mu);
          reader_err_msg = "empty value for " + wkey;
          reader_error.store(true);
          return;
        }
        ++local;
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int w = 0; w < n_writers; ++w) {
    threads.emplace_back([&, w]() {
      tinylsm::WriteOptions local_wo;
      local_wo.sync = false;
      const std::string big(64, static_cast<char>('a' + (w % 26)));
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string key =
            "w" + std::to_string(w) + "_" + std::to_string(local % 256);
        tinylsm::Status s = db->Put(local_wo, key, big);
        if (!s.ok()) {
          std::lock_guard<std::mutex> g(err_mu);
          writer_err_msg = s.ToString();
          writer_error.store(true);
          return;
        }
        ++local;
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_release);

  const auto hard =
      std::chrono::steady_clock::now() + std::chrono::seconds(kJoinGraceSeconds);
  ASSERT_TRUE(JoinAllWithDeadline(threads, hard))
      << "C1: worker join timed out (possible deadlock/hang)";

  EXPECT_FALSE(reader_error.load()) << reader_err_msg;
  EXPECT_FALSE(writer_error.load()) << writer_err_msg;
  EXPECT_GT(reads.load(), 0u);
  EXPECT_GT(writes.load(), 0u);
  (void)impl->TEST_WaitForFlush();
}

// ---------------------------------------------------------------------------
// C2: Write stall when imm pending — many writers, tiny buffer; no deadlock.
// ---------------------------------------------------------------------------
TEST_F(DbConcurrentTest, C2_WriteStallManyWritersNoDeadlock) {
  const int n_writers = StressThreads();
  const int seconds = std::min(StressSeconds(), 10);

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(
      tinylsm::DB::Open(SmallBufferOptions(/*write_buffer=*/200), dbname_, &db)
          .ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> puts{0};
  std::atomic<bool> writer_error{false};
  std::string err_msg;
  std::mutex err_mu;

  std::vector<std::thread> threads;
  for (int w = 0; w < n_writers; ++w) {
    threads.emplace_back([&, w]() {
      tinylsm::WriteOptions wo;
      wo.sync = false;
      const std::string payload(96, static_cast<char>('A' + (w % 26)));
      uint64_t i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string key =
            "stall_w" + std::to_string(w) + "_" + std::to_string(i);
        tinylsm::Status s = db->Put(wo, key, payload);
        if (!s.ok()) {
          std::lock_guard<std::mutex> g(err_mu);
          err_msg = s.ToString();
          writer_error.store(true);
          return;
        }
        ++i;
        puts.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_release);

  const auto hard =
      std::chrono::steady_clock::now() + std::chrono::seconds(kJoinGraceSeconds);
  ASSERT_TRUE(JoinAllWithDeadline(threads, hard))
      << "C2: writers did not unblock within grace period (deadlock?)";

  EXPECT_FALSE(writer_error.load()) << err_msg;
  EXPECT_GT(puts.load(), 0u);
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  tinylsm::ReadOptions ro;
  std::string value;
  ASSERT_TRUE(db->Get(ro, "stall_w0_0", &value).ok());
  EXPECT_FALSE(value.empty());
}

// ---------------------------------------------------------------------------
// C3: Compaction unlink vs old Version reader (TEST hooks).
// Hold an old Version while concurrent Gets run and compaction finishes;
// input SSTs stay until the hold is released, then purge unlinks.
// ---------------------------------------------------------------------------
TEST_F(DbConcurrentTest, C3_CompactionUnlinkVsOldVersionReader) {
  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(
                  SmallBufferOptions(/*write_buffer=*/256, /*l0_trigger=*/2),
                  dbname_, &db)
                  .ok());
  std::unique_ptr<tinylsm::DB> holder(db);
  auto* impl = AsImpl(db);

  tinylsm::WriteOptions wo;
  wo.sync = false;
  tinylsm::ReadOptions ro;

  ASSERT_TRUE(db->Put(wo, "pin", "v0").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  ASSERT_TRUE(db->Put(wo, "pin", "v1").ok());
  ASSERT_TRUE(impl->TEST_ForceFreeze().ok());
  ASSERT_TRUE(impl->TEST_WaitForFlush());

  std::shared_ptr<tinylsm::Version> held = impl->TEST_CurrentVersion();
  ASSERT_NE(held, nullptr);
  ASSERT_GE(held->NumFiles(0), 2u);

  std::vector<uint64_t> input_numbers;
  for (const auto& f : held->LevelFiles(0)) {
    input_numbers.push_back(f->number);
    ASSERT_TRUE(impl->TEST_SstFileExists(f->number));
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> reader_error{false};
  std::string err_msg;
  std::mutex err_mu;
  const int n_readers = std::max(2, StressThreads() / 2);

  std::vector<std::thread> readers;
  for (int r = 0; r < n_readers; ++r) {
    readers.emplace_back([&]() {
      std::string value;
      while (!stop.load(std::memory_order_relaxed)) {
        tinylsm::Status s = db->Get(ro, "pin", &value);
        if (!s.ok()) {
          std::lock_guard<std::mutex> g(err_mu);
          err_msg = s.ToString();
          reader_error.store(true);
          return;
        }
        if (value != "v0" && value != "v1") {
          std::lock_guard<std::mutex> g(err_mu);
          err_msg = "unexpected pin value: " + value;
          reader_error.store(true);
          return;
        }
      }
    });
  }

  ASSERT_TRUE(impl->TEST_WaitForCompaction());
  EXPECT_EQ(impl->TEST_NumL0Files(), 0);
  EXPECT_GE(impl->TEST_NumL1Files(), 1);

  for (uint64_t n : input_numbers) {
    EXPECT_TRUE(impl->TEST_SstFileExists(n))
        << "input " << n << " unlinked while old Version held";
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }
  EXPECT_FALSE(reader_error.load()) << err_msg;

  held.reset();
  impl->TEST_PurgeObsoleteFiles();
  for (uint64_t n : input_numbers) {
    EXPECT_FALSE(impl->TEST_SstFileExists(n))
        << "input " << n << " should be unlinked after last Version drop";
  }

  std::string value;
  ASSERT_TRUE(db->Get(ro, "pin", &value).ok());
  EXPECT_EQ(value, "v1");
}

// ---------------------------------------------------------------------------
// C4: Disjoint-key stress + model map. Each thread owns a key range; after
// join, DB must match the locked model for every successfully written key.
// ---------------------------------------------------------------------------
TEST_F(DbConcurrentTest, C4_DisjointKeyStressModelMap) {
  const int n_threads = StressThreads();
  const int seconds = StressSeconds();

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(tinylsm::DB::Open(
                  SmallBufferOptions(/*write_buffer=*/1024, /*l0_trigger=*/4),
                  dbname_, &db)
                  .ok());
  std::unique_ptr<tinylsm::DB> holder(db);

  // Model: key -> value if present; nullopt means deleted after a successful Delete.
  std::mutex model_mu;
  std::map<std::string, std::optional<std::string>> model;

  std::atomic<bool> stop{false};
  std::atomic<bool> worker_error{false};
  std::string err_msg;
  std::mutex err_mu;
  std::atomic<uint64_t> ops{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < n_threads; ++t) {
    threads.emplace_back([&, t]() {
      tinylsm::WriteOptions wo;
      wo.sync = false;
      constexpr int kKeysPerThread = 64;
      uint64_t seq = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const int k = static_cast<int>(seq % kKeysPerThread);
        const std::string key =
            "t" + std::to_string(t) + "_k" + std::to_string(k);
        const int op = static_cast<int>(seq % 5);  // bias toward Put
        tinylsm::Status s;
        if (op == 0) {
          s = db->Delete(wo, key);
          if (!s.ok()) {
            std::lock_guard<std::mutex> g(err_mu);
            err_msg = s.ToString();
            worker_error.store(true);
            return;
          }
          {
            std::lock_guard<std::mutex> g(model_mu);
            model[key] = std::nullopt;
          }
        } else {
          const std::string val =
              "v_t" + std::to_string(t) + "_" + std::to_string(seq);
          s = db->Put(wo, key, val);
          if (!s.ok()) {
            std::lock_guard<std::mutex> g(err_mu);
            err_msg = s.ToString();
            worker_error.store(true);
            return;
          }
          {
            std::lock_guard<std::mutex> g(model_mu);
            model[key] = val;
          }
        }
        ++seq;
        ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_release);

  const auto hard =
      std::chrono::steady_clock::now() + std::chrono::seconds(kJoinGraceSeconds);
  ASSERT_TRUE(JoinAllWithDeadline(threads, hard))
      << "C4: worker join timed out (possible deadlock/hang)";

  ASSERT_FALSE(worker_error.load()) << err_msg;
  EXPECT_GT(ops.load(), 0u);

  auto* impl = AsImpl(db);
  ASSERT_TRUE(impl->TEST_WaitForFlush());
  (void)impl->TEST_WaitForCompaction();

  tinylsm::ReadOptions ro;
  std::string value;
  std::lock_guard<std::mutex> g(model_mu);
  for (const auto& kv : model) {
    const std::string& key = kv.first;
    const std::optional<std::string>& expected = kv.second;
    tinylsm::Status s = db->Get(ro, key, &value);
    if (!expected.has_value()) {
      EXPECT_TRUE(s.IsNotFound())
          << "key " << key << " should be deleted: " << s.ToString();
    } else {
      ASSERT_TRUE(s.ok()) << "key " << key << ": " << s.ToString();
      EXPECT_EQ(value, *expected) << "model mismatch for " << key;
    }
  }
}

// ---------------------------------------------------------------------------
// C5: Same-key multi-writer smoke. Many threads Put the same keys; no crash
// or deadlock. Weak check: each Get returns some non-empty value (no
// linearizability claim — TSan-clean expectation for a later CI job).
// ---------------------------------------------------------------------------
TEST_F(DbConcurrentTest, C5_SameKeyMultiWriterSmoke) {
  const int n_threads = StressThreads();
  const int seconds = std::min(StressSeconds(), 8);

  tinylsm::DB* db = nullptr;
  ASSERT_TRUE(
      tinylsm::DB::Open(SmallBufferOptions(/*write_buffer=*/512), dbname_, &db)
          .ok());
  std::unique_ptr<tinylsm::DB> holder(db);

  constexpr int kSharedKeys = 16;
  std::atomic<bool> stop{false};
  std::atomic<bool> worker_error{false};
  std::atomic<uint64_t> puts{0};
  std::string err_msg;
  std::mutex err_mu;

  std::vector<std::thread> threads;
  for (int t = 0; t < n_threads; ++t) {
    threads.emplace_back([&, t]() {
      tinylsm::WriteOptions wo;
      wo.sync = false;
      uint64_t i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const int k = static_cast<int>(i % kSharedKeys);
        const std::string key = "shared" + std::to_string(k);
        const std::string val =
            "t" + std::to_string(t) + "_i" + std::to_string(i);
        tinylsm::Status s = db->Put(wo, key, val);
        if (!s.ok()) {
          std::lock_guard<std::mutex> g(err_mu);
          err_msg = s.ToString();
          worker_error.store(true);
          return;
        }
        ++i;
        puts.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_release);

  const auto hard =
      std::chrono::steady_clock::now() + std::chrono::seconds(kJoinGraceSeconds);
  ASSERT_TRUE(JoinAllWithDeadline(threads, hard))
      << "C5: worker join timed out (possible deadlock/hang)";

  ASSERT_FALSE(worker_error.load()) << err_msg;
  EXPECT_GT(puts.load(), 0u);

  tinylsm::ReadOptions ro;
  std::string value;
  for (int k = 0; k < kSharedKeys; ++k) {
    const std::string key = "shared" + std::to_string(k);
    tinylsm::Status s = db->Get(ro, key, &value);
    ASSERT_TRUE(s.ok()) << key << ": " << s.ToString();
    EXPECT_FALSE(value.empty()) << key;
  }
}

TEST_F(DbConcurrentTest, StressConfigDefaultsAreSane) {
  EXPECT_GE(StressThreads(), 2);
  EXPECT_LE(StressThreads(), 32);
  EXPECT_GE(StressSeconds(), 1);
  EXPECT_LE(StressSeconds(), 120);
}

}  // namespace
