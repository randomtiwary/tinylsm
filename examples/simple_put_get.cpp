// Minimal tinylsm example: Open, Put, Get, Delete.
//
// Build (from build tree): targets `simple_put_get` when TINYLSM_BUILD_EXAMPLES=ON
//   cmake -S . -B build -DTINYLSM_BUILD_EXAMPLES=ON && cmake --build build
//   ./build/examples/simple_put_get /tmp/tinylsm-demo

#include "tinylsm/db.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const std::string dbpath =
      (argc > 1) ? argv[1] : std::string("/tmp/tinylsm-simple-example");

  tinylsm::Options options;
  options.create_if_missing = true;
  // Optional educational bloom on flushes (0 = off).
  options.bloom_bits_per_key = 10;

  tinylsm::DB* db = nullptr;
  tinylsm::Status s = tinylsm::DB::Open(options, dbpath, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  tinylsm::WriteOptions wo;
  wo.sync = true;

  s = db->Put(wo, "hello", "world");
  if (!s.ok()) {
    std::fprintf(stderr, "Put failed: %s\n", s.ToString().c_str());
    delete db;
    return 1;
  }

  std::string value;
  s = db->Get(tinylsm::ReadOptions(), "hello", &value);
  if (!s.ok()) {
    std::fprintf(stderr, "Get failed: %s\n", s.ToString().c_str());
    delete db;
    return 1;
  }
  std::printf("Get(hello) = %s\n", value.c_str());

  s = db->Delete(wo, "hello");
  if (!s.ok()) {
    std::fprintf(stderr, "Delete failed: %s\n", s.ToString().c_str());
    delete db;
    return 1;
  }

  s = db->Get(tinylsm::ReadOptions(), "hello", &value);
  if (s.IsNotFound()) {
    std::printf("Get(hello) after Delete: NotFound (ok)\n");
  } else if (!s.ok()) {
    std::fprintf(stderr, "Get after Delete failed: %s\n", s.ToString().c_str());
    delete db;
    return 1;
  } else {
    std::fprintf(stderr, "expected NotFound after Delete, got value=%s\n",
                 value.c_str());
    delete db;
    return 1;
  }

  delete db;
  std::printf("simple_put_get: success (db at %s)\n", dbpath.c_str());
  return 0;
}
