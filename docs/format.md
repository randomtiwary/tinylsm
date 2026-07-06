# TinyLSM on-disk byte formats

**Status:** Normative. Frozen for implementers of PR 8+ (WAL, SST, MANIFEST/recovery, and later subsystems).  
**On-disk format version:** `TINYLSM1` (SST footer magic).  
**Source of truth:** Design document sections on internal keys, WAL, SSTable, and Version/MANIFEST.  
**Endianness (global rule):** All multi-byte integers on disk and in internal-key trailers use **little-endian (LE)** unless an explicit exception is documented. Varints use standard unsigned **LEB128** (LevelDB-style).

This document freezes **wire layouts only**. Recovery algorithms, lock protocols, and compaction policy live in the design document; implementers must not invent alternate layouts for the structures specified here.

---

## 1. File naming

| Kind | Pattern | Notes |
|------|---------|--------|
| WAL | `{number}.log` | Decimal integer; no required zero-pad width for correctness (helpers may pad for readability, e.g. 6 digits). Parse trailing path component as `^[0-9]+\.log$`. |
| SSTable | `{number}.sst` | Same numeric convention. Write via `{number}.sst.tmp` (or Env temp), then rename. |
| SST temp | `*.sst.tmp` | Not live data; may contribute to max file-number scan on Open. |
| Manifest | `MANIFEST-{number}` | e.g. `MANIFEST-1`. |
| Current pointer | `CURRENT` | Single-line filename of the live MANIFEST (+ optional trailing newline). |
| Lock | `LOCK` | Directory lock file (not a versioned format). |

File numbers are non-negative integers allocated by the VersionSet allocator (`NewFileNumber` / `PeekNextFileNumber`). Do not recycle log file numbers.

---

## 2. CRC-32C and LevelDB-compatible mask

Used by **WAL frames**, **SST block trailers**, and **MANIFEST frames**. Same algorithm and mask on every path.

### 2.1 Algorithm

- **CRC-32C** (Castagnoli polynomial `0x1EDC6F41`), same intent as LevelDB/RocksDB for hardware-friendly CRC.
- Protected bytes are path-specific (see each section); the CRC is always computed over those bytes, then **masked** before storage.

### 2.2 Mask / unmask (all `uint32_t` modular arithmetic)

**Mask** (before store as `fixed32_le`):

```text
masked = ((crc >> 15) | (crc << 17)) + 0xa282ead8u
```

**Unmask** (mandatory on every read path):

```text
unmasked = masked - 0xa282ead8u
crc      = (unmasked << 15) | (unmasked >> 17)
```

**Verify:** recompute `crc32c` over the protected bytes and compare to unmasked `crc`.

Implementers must include mask→unmask round-trip tests and at least one known-vector pair in unit tests (PR 3 / CRC PR).

---

## 3. Internal key encoding

User keys alone are insufficient for overwrites and deletes. Every memtable/SSTable entry uses an **internal key**:

```text
internal_key = user_key_bytes || fixed64_le( (sequence << 8) | type )
```

| Field | Encoding |
|-------|----------|
| `user_key` | Raw bytes (no length prefix inside the internal key itself; lengths appear in block/WAL encodings that embed the key) |
| Trailer | 8-byte little-endian `uint64_t` |
| `type` (low 8 bits) | `kTypeDeletion = 0`, `kTypeValue = 1` |
| `sequence` (high 56 bits) | Monotonic `uint64_t` sequence; effective max `2^56 - 1` |

### 3.1 Comparator (`CompareInternalKey`)

1. Compare `user_key` with raw byte-wise ascending order (`memcmp` / string compare).
2. If equal, compare `sequence` **descending** (higher sequence first).
3. If still equal, compare `type` **descending** as unsigned 8-bit.

### 3.2 Malformed keys

Internal keys shorter than 8 bytes are **corrupt**. Builders never emit them. SST readers return `Corruption` if found. Memtable assumes well-formed inserts from `DBImpl` only.

### 3.3 LookupKey (Get seek form)

```text
MakeLookupKey(user_key, snapshot_seq):
  user_key || fixed64_le( (snapshot_seq << 8) | kTypeValue )
```

With sequence-descending comparator order, seeking this finds the newest entry with `seq <= snapshot_seq` for that user key when iterating forward.

---

## 4. WAL (Write-Ahead Log)

**Purpose:** Durable record of unflushed memtable updates across one or more log files.  
**v1:** One physical record = one logical Put or Delete. **No** LevelDB-style first/middle/last fragmentation. **No** separate physical “type” byte on the frame; the payload’s `value_type` distinguishes value vs deletion.

### 4.1 Physical record layout

```text
offset 0:  fixed32_le length            // number of bytes in `payload` only
offset 4:  fixed32_le crc32c_masked     // CRC of `payload` only, then masked (§2)
offset 8:  payload[length]
```

Layout diagram:

```text
+----------------+----------------------+---------------------+
| length (u32 LE)| crc32c_masked (u32 LE)| payload[length]    |
+----------------+----------------------+---------------------+
 0              4                      8               8+length
```

### 4.2 Payload encoding

```text
fixed64_le sequence
u8         value_type        // 0 = deletion, 1 = value
varint32   key_len
key_bytes[key_len]           // user key (not internal key)
varint32   val_len           // 0 for deletion
val_bytes[val_len]
```

### 4.3 Writer behavior (format-relevant)

- Append a full framed record without interleaving other writers’ frames (DB mutex during WAL write).
- If `WriteOptions.sync == true` (or options default durable mode): `fdatasync` the log fd (or `fsync` if `fdatasync` unavailable).
- After **creating** a new log file: best-effort `fsync` of the parent directory so the directory entry is durable.
- Do not recycle log file numbers.

### 4.4 Reader behavior (recovery scan) — completeness vs corruption

For each log file, read sequentially:

1. If fewer than 8 bytes remain → **stop successfully** (clean EOF or empty file).
2. Read `length`, `crc_masked`. If the file lacks a full 8-byte header, or `8 + length` would exceed file size → **stop successfully** (torn tail; drop incomplete final record). **Not** `Corruption`.
3. Read full `payload[length]`. Unmask CRC and verify over `payload` only.
   - **Full payload read + CRC mismatch → `Status::Corruption`**, even if this is the last record at EOF. A complete framed record with a bad CRC is bitrot or a bug, not a tear.
4. Decode payload; yield `(seq, type, key, value)`.

---

## 5. SSTable (`*.sst`) — file layout

**File order (v1):**

```text
[ data block 0 ][ data block 1 ]...[ data block N-1 ]
[ meta / filter block? ]   // absent in v1 if bloom disabled (filter_handle = 0,0)
[ index block ]
[ footer ]                 // fixed 48 bytes at EOF
```

With bloom disabled (v1 default): **no filter block** — only data blocks, then index block, then footer.

**Empty table:** `TableBuilder::Finish` on zero keys still writes a valid file: prefer **one empty data block** + index block with **zero** entries + valid footer. Reader returns `NotFound` for all Gets.

### 5.1 Block trailer (every block: data, index, filter)

Each block on disk is:

```text
block_contents[length]
u8  compression_type     // always 0 = kNoCompression in v1
fixed32_le crc32c_masked // CRC over (block_contents || compression_type byte), then masked (§2)
```

```text
+----------------------+----+----------------------+
| block_contents       | CT | crc32c_masked (u32 LE)|
+----------------------+----+----------------------+
                         ^ 5-byte trailer
```

**BlockHandle (normative only):** `BlockHandle = (offset, size)` refers to **`block_contents` bytes only** — **not** including the 5-byte trailer.

- Reader reads `size` bytes at `offset`, then reads the 5-byte trailer at `offset + size`.
- Index entries and footer handles always use this convention.

### 5.2 Data block contents (v1 — raw keys, restart array)

**No prefix compression in v1.** Entry format:

```text
varint32 key_len
key_bytes[key_len]           // full internal key (§3)
varint32 value_len
value_bytes[value_len]
```

**Restart points:** every `kBlockRestartInterval = 16` entries, record the offset (from start of `block_contents`) of that entry in a restart array. Restarts enable binary search even without prefix compression (seek to restart, linear scan within interval).

**`block_contents` layout:**

```text
[entries...]
fixed32_le restart_offset[0]
fixed32_le restart_offset[1]
...
fixed32_le restart_offset[R-1]
fixed32_le num_restarts        // R
```

```text
+------------------+-------------------+ ... +-------------------+----------------+
| entries...       | restart[0] (u32 LE)|     | restart[R-1]      | num_restarts   |
+------------------+-------------------+ ... +-------------------+----------------+
```

### 5.3 Index block

Uses the **same block entry encoding** as data blocks:

| Field | Meaning |
|-------|---------|
| **key** | Separator internal key. **Normative v1:** the **last internal key of the data block** (full key; not a shortest separator). |
| **value** | Encoded `BlockHandle`: `varint64 offset \| varint64 size` (contents size only; see §5.1). |

Binary search in the index finds the appropriate data block for a lookup; then load that data block and search within it.

### 5.4 Footer (exactly 48 bytes at EOF)

```text
offset from EOF-48:
  0:  fixed64_le index_handle.offset
  8:  fixed64_le index_handle.size      // size of index block_contents
 16:  fixed64_le filter_handle.offset   // 0 if no filter
 24:  fixed64_le filter_handle.size     // 0 if no filter
 32:  fixed64_le padding_or_version     // must be 0 for TINYLSM1
 40:  fixed64_le magic                  // see below
```

```text
+--------+--------+--------+--------+--------+--------+
| idx_off| idx_sz | fil_off| fil_sz | pad=0  | magic  |
| 8B LE  | 8B LE  | 8B LE  | 8B LE  | 8B LE  | 8B     |
+--------+--------+--------+--------+--------+--------+
 0        8       16       24       32       40      48
```

**Magic (both forms normative):**

| Form | Value |
|------|--------|
| ASCII / file bytes | `TINYLSM1` = `0x54 0x49 0x4E 0x59 0x4C 0x53 0x4D 0x31` |
| LE `uint64_t` on LE host | Interpret those 8 bytes in **file order** (do **not** byte-swap the ASCII). Readers compare the 8-byte sequence to `"TINYLSM1"`. |

**Bloom (later PR):** non-zero `filter_handle`; **same magic `TINYLSM1`**. Zero `filter_handle` means “no filter”. No format bump required for optional bloom.

### 5.5 Block size policy

`TableBuilder` starts a new data block when `current_block_bytes >= Options::block_size` (default **4096**) **after** finishing an entry (never split an entry across blocks).

### 5.6 Builder durability (publish path)

1. Write to `{number}.sst.tmp` (or Env temp).
2. `fdatasync` the file.
3. `rename` to `{number}.sst`.
4. Best-effort `fsync` of the parent directory.
5. On `Finish` failure: **delete** partial temp/SST file.
6. `LogAndApply` only after successful durable SST publish.

### 5.7 Reader concurrency (format-relevant)

Immutable file; **`pread` + heap buffer** (not mmap in v1). No block cache required in first SST PRs.

---

## 6. VersionEdit (logical payload)

A `VersionEdit` is a concatenation of tagged fields (LevelDB-inspired). Each field:

```text
varint32 tag
// then type-specific body
```

| Tag | Name | Body |
|-----|------|------|
| 1 | `kComparator` | `varint32 len \| bytes`. **Normative name:** `tinylsm.BytewiseComparator` |
| 2 | `kLogNumber` | `varint64 log_number` |
| 3 | `kNextFileNumber` | `varint64` |
| 4 | `kLastSequence` | `varint64` |
| 5 | `kCompactPointer` | **Unused in v1** (skip if present for forward compat) |
| 6 | `kDeletedFile` | `varint32 level \| varint64 file_number` |
| 7 | `kNewFile` | `varint32 level \| varint64 file_number \| varint64 file_size \| encoded smallest internal key \| encoded largest internal key` |
| 9 | `kPrevLogNumber` | **Unused in v1** (ignore if present) |

**Encoded internal key** (for `kNewFile` smallest/largest):

```text
varint32 len
bytes[len]              // full internal key (§3)
```

Tag **8** is unused in this format version (reserved / not defined for v1).

---

## 7. MANIFEST physical framing

MANIFEST is an **append-only** file of records. Framing is **symmetric with WAL** (CRC of payload only, same mask):

```text
fixed32_le record_len
fixed32_le crc32c_masked(payload)   // CRC of payload only, then masked (§2)
payload[record_len]                 // one VersionEdit byte stream (§6)
```

```text
+-------------------+----------------------+---------------------------+
| record_len (u32 LE)| crc32c_masked (u32 LE)| VersionEdit payload      |
+-------------------+----------------------+---------------------------+
```

**CRC on MANIFEST: mandatory** (same mask/unmask as WAL and SST trailers).

---

## 8. CURRENT atomic update

`CURRENT` names the live MANIFEST file. Updates must be atomic:

1. Write file `CURRENT.tmp` containing exactly: `MANIFEST-{n}\n` (filename + newline).
2. `fsync` the temp file.
3. `rename` over `CURRENT`.
4. Best-effort `fsync` of the parent directory.
5. **Never** truncate `CURRENT` in place.

On Open, read `CURRENT` as a single line filename + optional newline; open that MANIFEST and replay all VersionEdit records.

---

## 9. NewDB seed (format outcomes)

When creating a new DB (`create_if_missing`, no `CURRENT`):

1. Empty in-memory Version.
2. Write `MANIFEST-1` with one VersionEdit containing at least:
   - `kComparator` → `tinylsm.BytewiseComparator`
   - `kLogNumber` → `1`
   - `kNextFileNumber` → `2`
   - `kLastSequence` → `0`
3. Atomically install `CURRENT` → `MANIFEST-1` (§8).
4. Create empty (or header-only, ready for append) `1.log`.
5. Best-effort directory `fsync`.

In-process init must match: `current_log_number_ = 1`, `next_file_number_ = 2`, `last_sequence_ = 0`, open WAL writer on `1.log`.

---

## 10. Quick reference: shared primitives

| Primitive | Definition |
|-----------|------------|
| `fixed32_le` / `fixed64_le` | Little-endian unsigned fixed width |
| `varint32` / `varint64` | Unsigned LEB128 |
| `crc32c_masked` | CRC-32C then LevelDB mask (§2) |
| Internal key trailer | `fixed64_le((seq << 8) \| type)` |
| Value types | `0` deletion, `1` value |
| SST magic | ASCII `TINYLSM1` at footer offset 40 |
| Footer length | **48** bytes |
| Block trailer length | **5** bytes (`compression_type` + masked CRC) |
| `BlockHandle.size` | Size of **contents only** (exclude 5-byte trailer) |
| Restart interval | **16** entries |
| Default data block target | **4096** bytes (`Options::block_size`) |

---

## 11. Implementer checklist (formats only)

- [ ] WAL frame: 8-byte header + payload; CRC over payload only; mask on write, unmask on read
- [ ] WAL torn tail stops cleanly; full-frame CRC mismatch is `Corruption`
- [ ] SST every block has 5-byte trailer; handles exclude trailer
- [ ] Data blocks use full internal keys, restart array at end of contents, `num_restarts` last
- [ ] Index key = last internal key of data block; value = varint64 offset + varint64 size
- [ ] Footer exactly 48 bytes; magic `TINYLSM1`; unused fields zero when no filter
- [ ] VersionEdit tags 1–4, 6–7 as specified; ignore 5 and 9 if present
- [ ] MANIFEST framing identical shape to WAL (len + masked CRC + payload)
- [ ] `CURRENT` updated only via temp + fsync + rename
- [ ] All multi-byte integers LE; no invented endianness or alternate magic

End of format freeze. Behavioral recovery rules (e.g. multi-log replay, always-apply WAL records) are normative in the design document but do not alter these layouts.
