# TinyLSM concurrency model (v1)

**Status:** Educational summary of the locking model implemented in `DBImpl`.
The design document §10 is normative; this note is a short map for readers of
the stress tests (`tests/db_concurrent_test.cpp`).

## Single lock authority

All mutable DB state is protected by **`DBImpl::mutex_`**:

| Resource | Protection |
|----------|------------|
| Active memtable (`mem_`) Add / Get | `mutex_` held |
| Immutable memtable (`imm_`) Get | `mutex_` held (no writers after freeze) |
| WAL append (+ optional sync) | `mutex_` held (serializes writers; educational) |
| Sequence allocation (`last_sequence_`) | `mutex_` held |
| Version publish (`LogAndApply` / `current_`) | `mutex_` held |
| BG scheduling flags, `bg_error_` | `mutex_` held |
| SST build / compaction merge IO | **mutex released** |
| SST reads on a published Version | **`std::shared_ptr<Version>` held; mutex not held** |
| Obsolete SST unlink | Only when no `shared_ptr<FileMetaData>` / Version still refs the file |

`VersionSet` has **no nested mutex**. Callers always hold `DBImpl::mutex_` when
mutating the version set.

## Get path

1. Lock `mutex_`.
2. Fail if sticky `bg_error_`.
3. Sample `snapshot_seq = last_sequence_`; build `LookupKey`.
4. Search **active** then **immutable** memtable **under the lock** (avoids
   concurrent `Add` races on the skiplist).
5. Copy `std::shared_ptr<Version> current`.
6. **Unlock**, then search L0 (newest-first) and L1 SSTs via that Version.

Holding the Version `shared_ptr` (and the `shared_ptr<FileMetaData>` entries it
owns) keeps SST files alive for the duration of the read even if a concurrent
compaction publishes a newer Version and registers the old inputs as obsolete.

## Put / Delete path and write stall

1. Lock with `unique_lock`.
2. `MakeRoomForWrite`:
   - If `imm_ != nullptr` and active mem is full → **write stall**:
     `background_work_finished_cv_.wait(lock)` (releases mutex while waiting).
   - If mem full and `imm_ == nullptr` → freeze: `mem_ → imm_`, new empty mem,
     new WAL file number; schedule BG flush. **Manifest `log_number` does not
     advance at freeze.**
3. WAL append, optional sync, `mem_->Add`, bump `last_sequence_`.
4. Optionally freeze again if the write filled the mem.

## Single background worker

One BG thread handles **flush** (prefer) then **compaction**:

- Claim work under `mutex_`, set `bg_working_`.
- **Unlock** for SST build / compaction IO.
- Re-lock for `LogAndApply`, drop `imm_` on successful flush, notify write-stall
  waiters, schedule compaction if L0 ≥ trigger.
- Sticky `bg_error_` fails subsequent Puts/Gets; BG stops thrashing on the
  failed claim.

## Version lifetime and unlink (C3)

- Published state is a COW `std::shared_ptr<Version>`.
- Each level lists `std::shared_ptr<FileMetaData>`.
- After compaction apply, input metas are registered as `weak_ptr` in
  `pending_obsolete_sst_`.
- Unlink runs only when the weak_ptr has **expired** (no live Version still
  shares that meta). Tests use `TEST_CurrentVersion` to pin an old Version.

## Stress tests (PR 15)

| ID | What it stresses |
|----|------------------|
| **C1** | Concurrent Get while writers force flushes (small `write_buffer_size`) |
| **C2** | Many writers hitting write stall; must unblock (timeout = fail) |
| **C3** | Held old Version keeps SST files during compaction; purge after release |
| **C4** | Disjoint key ranges + locked model map; exact match after join |
| **C5** | Same-key multi-writer smoke; no crash/deadlock; weak non-empty Get |

CI-friendly defaults: **6 threads**, **8 seconds**. Override with:

```bash
export TINYLSM_STRESS_THREADS=8
export TINYLSM_STRESS_SECONDS=12
```

TSan coverage for these tests lands in a later CI PR; the locking model above is
what makes a clean TSan run possible without suppressions.
