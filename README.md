# tinylsm

Educational **LSM-tree** embedded key-value storage engine in **C++17**.

TinyLSM is a from-scratch, single-process embedded KV store intended for learning
how production-style engines (LevelDB / RocksDB lineage) are put together:
MemTable + WAL, SSTables, manifest/version set, recovery, and compaction — with
an explicit focus on **correctness, clarity, and small reviewable changes**.

## Features (v0.1)

| Area | What you get |
|------|----------------|
| **API** | `DB::Open`, `Put`, `Get`, `Delete` over byte-string keys/values |
| **MemTable** | Ordered skiplist; approximate flush by `Options::write_buffer_size` |
| **WAL** | Framed records + CRC; multi-log recovery from manifest `log_number` |
| **SSTables** | `TINYLSM1` footer; data + index blocks; mandatory block CRC trailers |
| **Bloom (optional)** | Whole-table filter block when `Options::bloom_bits_per_key > 0` (e.g. 10); `filter_handle = 0` means off (old tables still open) |
| **Versions** | MANIFEST / CURRENT; COW `VersionSet`; file metadata with key ranges |
| **Recovery** | Replay every WAL with number ≥ manifest log number, ascending |
| **Compaction** | Simple L0→L1 leveled strategy; tombstone drop at bottommost when covered |
| **Concurrency** | Single `DBImpl::mutex_`; SST IO off-lock via held `Version` refs; TSan CI job |
| **Env** | POSIX `pread`/files (no mmap in v1); directory `LOCK` |

## Goals

- Implement a minimal but complete educational LSM path: `Put` / `Get` / `Delete`
  over byte-string keys and values
- Prefer clear code and well-specified on-disk formats over micro-optimizations
- Make concurrency and durability learnable: locking model, WAL recovery, version
  publication, and tests that catch real bugs (including ThreadSanitizer in CI)
- Build incrementally with small PRs so each piece is reviewable

## Non-goals

- Production SLOs, multi-process sharing, or network/RPC protocol
- Matching RocksDB feature surface (column families, complex compaction policies,
  compression stacks, etc.)
- Peak throughput tuning as a primary success metric
- Copying or depending on other personal LSM experiments; this tree is greenfield
- Public range `Iterator` API in v0.1 (internal iterators exist for flush/compaction)

## Build

Requires **CMake 3.16+** and a **C++17** compiler.

```bash
cmake -S . -B build
cmake --build build
```

This produces a static library target `tinylsm`. Unit tests and the
`examples/simple_put_get` binary are built by default when tinylsm is the
top-level project (`TINYLSM_BUILD_TESTS` / `TINYLSM_BUILD_EXAMPLES`; pass
`OFF` to skip either).

```bash
# CMake 3.20+:
ctest --test-dir build --output-on-failure
# CMake 3.16–3.19:
cd build && ctest --output-on-failure
```

### Example

```bash
./build/examples/simple_put_get /tmp/tinylsm-demo
```

See [`examples/simple_put_get.cpp`](examples/simple_put_get.cpp) for a minimal
Open / Put / Get / Delete main linked against `tinylsm`.

### ThreadSanitizer

For race detection (GCC/Clang), use a dedicated build with `TINYLSM_TSAN=ON`
(`-fsanitize=thread -g -O1`). Concurrent stress defaults are CI-friendly; shorten
further for a quick local check:

```bash
cmake -S . -B build-tsan -DTINYLSM_TSAN=ON
cmake --build build-tsan
TINYLSM_STRESS_THREADS=4 TINYLSM_STRESS_SECONDS=5 \
  ctest --test-dir build-tsan -R Concurrent --output-on-failure
```

CI runs a full `ctest` under TSan with the same short stress env (and lowers
`vm.mmap_rnd_bits` so TSan’s shadow mapping works on modern kernels). Goal:
clean runs with **no** TSan suppressions. On a local host that prints
`unexpected memory mapping`, try
`sudo sysctl vm.mmap_rnd_bits=28` or run tests under `setarch $(uname -m) -R`.

## Layout

```
tinylsm/
  CMakeLists.txt
  include/tinylsm/   # public headers (db, options, status, env, …)
  src/               # library sources (table, bloom, memtable, WAL, …)
  tests/             # GoogleTest unit / stress tests
  examples/          # minimal programs linking tinylsm
  docs/              # design + on-disk format + concurrency
  .github/workflows/ # CI (build + ctest + TSan on Ubuntu)
  README.md
  LICENSE
```

## Documentation

| Doc | Contents |
|-----|----------|
| [docs/design.md](docs/design.md) | Full design: architecture, PR plan, recovery, locking, formats |
| [docs/format.md](docs/format.md) | Normative on-disk layouts (WAL, SST/`TINYLSM1`, MANIFEST/CURRENT) |
| [docs/concurrency.md](docs/concurrency.md) | Lock protocol, version refs, TSan policy |

**On-disk formats:** bloom uses a non-zero `filter_handle` without changing the
`TINYLSM1` magic; zero handle means “no filter”.

## License

MIT — see [LICENSE](LICENSE).
