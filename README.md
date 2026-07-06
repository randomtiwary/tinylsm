# tinylsm

Educational **LSM-tree** embedded key-value storage engine in **C++17**.

TinyLSM is a from-scratch, single-process embedded KV store intended for learning
how production-style engines (LevelDB / RocksDB lineage) are put together:
MemTable + WAL, SSTables, manifest/version set, recovery, and compaction — with
an explicit focus on **correctness, clarity, and small reviewable changes**.

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

## Build

Requires **CMake 3.16+** and a **C++17** compiler.

```bash
cmake -S . -B build
cmake --build build
```

This produces a static library target `tinylsm`. Unit tests are built by default
when tinylsm is the top-level project (`TINYLSM_BUILD_TESTS`; pass
`-DTINYLSM_BUILD_TESTS=OFF` to skip).

```bash
# CMake 3.20+:
ctest --test-dir build --output-on-failure
# CMake 3.16–3.19:
cd build && ctest --output-on-failure
```

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
  include/tinylsm/   # public headers
  src/               # library sources
  tests/             # GoogleTest smoke / unit tests
  docs/              # on-disk format freeze (see format.md)
  .github/workflows/ # CI (build + ctest on Ubuntu)
  README.md
  LICENSE
```

**On-disk formats:** see [docs/format.md](docs/format.md) (WAL, SST/`TINYLSM1`, MANIFEST/CURRENT).

## License

MIT — see [LICENSE](LICENSE).
