/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * colstore.h -- a column store table's row groups (../../COLSTORE.md):
 * their metadata, built from rows by a bulk load, and read back by
 * a scan. The delta store is the table's keyless rel, as a row
 * store table's rows are.
 */
#ifndef SM3_COLSTORE_H
#define SM3_COLSTORE_H

#include "../catalog/internal.h"
#include "sm2.h"
#include "sm3.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sm3 {

/*
 * The relids of a column store table's keys (../COLSTORE.md §4.3,
 * ../COLSTORE2.md §4). An RG lock's is a lock-table name alone.
 */
constexpr uint16_t marker_relid = 2;
constexpr uint16_t validmap_relid = 3;
constexpr uint16_t filter_relid = 4;
constexpr uint16_t rglock_relid = 5;

/* The rows of a row group (../COLSTORE.md §7). */
constexpr uint32_t rg_max_rows = 10000;

/*
 * The longest varlena value a column store table takes, in octets,
 * its varlena header not counted (../COLSTORE.md §5.4): every value
 * is in its CG.
 */
constexpr size_t col_max_value = 2048;

/* The rows of a row group of cat's tables: fewer if a test set them. */
inline uint32_t rg_rows(const catalog_t *cat) {
  return cat->rg_rows ? cat->rg_rows : rg_max_rows;
}

/* The name of the _id CG's file, among the columns' attnums. */
constexpr const char *rg_id_name = "id";

/* One CG of a row group, as its RG key records it (§4.4). */
struct rg_col_t {
  int32_t attnum = 0;    /* 0-based; -1 for the _id CG */
  uint64_t size = 0;     /* the CG file's bytes */
  uint32_t crc = 0;      /* its CRC32C */
  sm2::cg_stats_t stats; /* its zonemap */
};

/*
 * A row group's metadata: the value of its RG key. A version past 0
 * holds the rows of version 0 not in dropped, in their order
 * (../COLSTORE2.md §6.4).
 */
struct rg_meta_t {
  uint32_t rows = 0;
  uint64_t id_min = 0, id_max = 0;
  std::vector<rg_col_t> cols; /* the _id CG first */
  uint32_t rows0 = 0;         /* version 0's rows; 0 in version 0 */
  std::string dropped;        /* a bit for each of rows0: not held here */
};

/* The version of file set fsid, and its row group (../COLSTORE2.md §3). */
inline uint32_t fsid_version(uint64_t fsid) {
  return static_cast<uint32_t>(fsid & 3);
}
inline uint64_t fsid_rg(uint64_t fsid) { return fsid >> 2; }

/* The newest version of a row group (../COLSTORE2.md §3). */
constexpr uint32_t rg_last_version = 3;

/*
 * The rno in version to of the row at rno in version from, of one row
 * group (../COLSTORE2.md §6.4): false if to does not hold the row.
 */
bool rg_map_rno(const rg_meta_t &from, uint32_t rno, const rg_meta_t &to,
                uint32_t &out);

/* Encode and decode an RG key's value; decode fails SM_ERR_CORRUPT. */
void rg_meta_encode(const rg_meta_t &meta, std::string &out);
int rg_meta_decode(std::string_view in, rg_meta_t &meta);

/* The file name of attnum's CG: "id" for -1, else the attnum. */
std::string rg_file_name(int32_t attnum);

/*
 * A row group being built from rows: each added row's _id and
 * values, held until rg_build_flush writes them (../COLSTORE.md
 * §9.2). The memory is one row group's.
 */
struct rg_build_t {
  table_t *table = nullptr;
  /* Set: the next flush writes this version of a row group, with
     these rows0 and dropped (../COLSTORE2.md §6.7); 0: a new one. */
  uint64_t fsid = 0;
  uint32_t rows0 = 0;
  std::string dropped;
  std::vector<uint64_t> ids;
  std::vector<std::vector<std::string>> vals; /* by column */
  std::vector<std::vector<bool>> nulls;       /* by column */
};

/* Begin building row groups for table. */
void rg_build_begin(rg_build_t &b, table_t *table);

/* Add a row; a value out of line is read back first. Returns 0 or -1. */
int rg_build_add(rg_build_t &b, uint64_t id,
                 std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull);

/*
 * Write the rows added since the last flush as one row group: draw
 * its rg_id, or take b.fsid, write its CG files, put its filter keys
 * and its RG key. Nothing to write is no error. Returns 0 or -1.
 */
int rg_build_flush(rg_build_t &b);

/*
 * A row group read for a scan: its metadata, the CGs of the columns
 * the scan reads, the rows its delete markers name, and the row it
 * is at.
 */
struct rg_read_t {
  uint64_t fsid = 0;
  rg_meta_t meta;
  std::vector<sm2::cg_t *> cgs; /* by column; nullptr: not read */
  /* By column: added since, no CG here, read as its missing value. */
  std::vector<bool> added;
  sm2::cg_t *ids = nullptr;
  std::vector<bool> deleted; /* by rno: its validmap's, or a marker's */
  uint32_t ndeleted = 0;     /* the bits set in deleted */
  uint32_t nmarkers = 0;     /* the rows of deleted its markers name */
  uint32_t at = 0;
  uint32_t last = 0; /* the rno of the row read last */
};

/* Free what r holds. */
void rg_read_close(rg_read_t &r);

/*
 * Read the row group of file set fsid, with metadata meta, for a
 * scan of table: the CGs of the columns in cols, every column if
 * cols is empty, and its delete markers at the transaction's view.
 * Returns 0 or -1.
 */
int rg_read_open(table_t *table, uint64_t fsid, rg_meta_t meta,
                 const std::vector<bool> &cols, rg_read_t &r);

/*
 * The next live row of r, as datums into values and isnull, one per
 * column of the table; a column not read is null. 0 with id and
 * r.last set, 1 at the end, or -1. The datums view r, until its next
 * call.
 */
int rg_read_next(table_t *table, rg_read_t &r, uint64_t &id,
                 std::span<sm2::datum_t> values, std::span<bool> isnull);

/*
 * Put the rows b holds into the delta store, under their _ids,
 * through insert: a bulk load's rows past its last row group
 * (../COLSTORE.md §9.2). Returns 0 or -1.
 */
int rg_build_to_delta(
    rg_build_t &b,
    const std::function<int(uint64_t, std::span<const sm2::datum_t>,
                            std::span<const bool>)> &insert);

/*
 * The row group part of a scan of a column store table: the row
 * groups its snapshot sees, read one at a time, each skipped when
 * the bounds prove no row of it matches, and an _id range.
 */
struct col_scan_t {
  table_t *table = nullptr;
  std::vector<std::pair<uint64_t, std::string>> rgs; /* (fsid, key value) */
  size_t next = 0;
  bool open = false;
  rg_read_t rg;
  std::vector<bool> cols; /* the columns read; empty: all */
  std::vector<col_bound_t> bounds;
  uint64_t id_lo = 0, id_hi = UINT64_MAX;
  std::vector<sm2::datum_t> values;
  std::unique_ptr<bool[]> isnull;
  std::string tuple;
};

/*
 * Open the row group part of a scan of table, at its transaction's
 * view: the columns cols, every one if empty; the rows that may meet
 * bounds; the _ids id_lo to id_hi. Returns 0 or -1.
 */
int col_scan_open(table_t *table, std::vector<bool> cols,
                  std::vector<col_bound_t> bounds, uint64_t id_lo,
                  uint64_t id_hi, col_scan_t *&out);

/*
 * The next row of the row groups: 0 with id and tuple, the row as
 * sm2's tuple, viewing c until its next call; 1 past the last row
 * group; -1.
 */
int col_scan_next(col_scan_t *c, uint64_t &id, std::string_view &tuple);

/* Close the row group part of a scan; nullptr is a no-op. */
void col_scan_close(col_scan_t *c);

/* The place of c's last row. */
inline place_t col_scan_place(const col_scan_t *c) {
  return {c->rg.fsid, c->rg.last};
}

/*
 * The rnos of the delete markers of file set fsid that the
 * transaction sees, into rnos: its rows deleted (../COLSTORE2.md §4).
 */
int col_markers(table_t *table, uint64_t fsid, std::vector<uint32_t> &rnos);

/*
 * The bytes of the CGs of the row groups the transaction sees, their
 * long-value files included, into bytes (§7).
 */
int col_bytes(table_t *table, uint64_t &bytes);

/*
 * Open a cursor over the delta store of a column store table alone,
 * in _id order, without its row groups. Close it with tbl_scanend.
 */
int tbl_scan_delta(table_t *table, scan_t *&out);

/* The key of the delete marker of the row at place (../COLSTORE2.md §4). */
std::string marker_key(place_t place);

/* The key of file set fsid's validmap (../COLSTORE2.md §4). */
std::string validmap_key(uint64_t fsid);

/* The lock-table name of row group rg_id (../COLSTORE2.md §4, §5). */
std::string rg_lock_key(uint64_t rg_id);

/*
 * Whether bit rno of a validmap's bytes is set: the row is deleted
 * (../COLSTORE2.md §4).
 */
inline bool validmap_test(std::string_view bits, uint32_t rno) {
  return rno / 8 < bits.size() &&
         (static_cast<unsigned char>(bits[rno / 8]) >> (rno % 8)) & 1;
}

/*
 * The rows of file set fsid that are deleted, as the transaction
 * sees them: the bits of its validmap, and its markers. Into dead,
 * a bit for each of its rows rows; ndead the bits set, and markers
 * those its markers name. Returns 0 or -1.
 */
int col_dead(table_t *table, uint64_t fsid, uint32_t rows,
             std::vector<bool> &dead, uint32_t &ndead, uint32_t &markers);

/*
 * The live file set of row group rg_id at the newest committed state,
 * into fsid and meta: 0, 1 if it has none, or -1. One read when it is
 * the version of fsid on entry.
 */
int col_newest_version(table_t *table, uint64_t rg_id, uint64_t &fsid,
                       rg_meta_t &meta);

/*
 * The metadata of file set fsid as the transaction sees it: 0, 1 if
 * it sees none, or -1.
 */
int col_meta_seen(table_t *table, uint64_t fsid, rg_meta_t &meta);

/*
 * Erase the delete markers, shadow markers among them, and the
 * validmap of file set fsid of table, an old version that RG
 * compaction replaced in place (../COLSTORE2.md §6.9), in the
 * caller's transaction: once no snapshot sees the version, as its
 * _id CG's filter key, erased in the compaction's commit, says. done
 * false, with nothing written, while one may. Returns 0 or -1.
 */
int col_cleanup(table_t *table, uint64_t fsid, bool &done);

/*
 * Fold the delete markers of file set fsid of table into its
 * validmap (../COLSTORE2.md §6.6), in the caller's transaction, at
 * the newest committed state: X on the row group, if it can be had
 * now; then a new validmap, with a bit for each marker, and the
 * markers erased. A writer of the row group in flight or prepared
 * makes it give up, done false, with nothing written. A file set
 * gone, or with no marker, is done with nothing written. Returns 0
 * or -1.
 */
int col_fold(table_t *table, uint64_t fsid, bool &done);

/*
 * The place of row id's live copy in a row group, at the newest
 * committed state, not the transaction's snapshot: a writer follows
 * a row the seal moved (../COLSTORE.md §10.2). 0 with place set, 1
 * if no row group holds it, or -1.
 */
int col_find_latest(table_t *table, uint64_t id, place_t &place);

/*
 * col_find_latest, as the transaction sees it: the place its own
 * reads give the row, which a writer hides by a shadow marker when it
 * wrote the row at another (../COLSTORE2.md §6.3). 0, 1, or -1.
 */
int col_find_seen(table_t *table, uint64_t id, place_t &place);

/*
 * Compact file set fsid of table, a row group's version below the
 * last, into its next version (../COLSTORE2.md §6.7), in the caller's
 * transaction, at the newest committed state: its live rows alone, in
 * their order; then the version's RG key and filter keys erased. A
 * writer of the row group makes it give up, done false, with nothing
 * left written; a holder of a row's lock, on its _id, does not.
 * Returns 0 or -1.
 */
int col_compact(table_t *table, uint64_t fsid, bool &done);

/* Put a live row of an eliminated row group into the delta store. */
using movein_fn = std::function<int(uint64_t id, std::span<const sm2::datum_t>,
                                    std::span<const bool>)>;

/*
 * Eliminate file set fsid of table, a row group's newest version
 * (../COLSTORE2.md §6.8), in the caller's transaction, at the newest
 * committed state: each live row given to movein, which puts it into
 * the delta store under its _id as a moved-in version; then the RG
 * key and filter keys erased. Its markers and validmap stay for the
 * snapshots that see it. A writer of the row group, or a holder of a
 * live row's lock, makes it give up, done false, with nothing left
 * written. Returns 0 or -1.
 */
int col_eliminate(table_t *table, uint64_t fsid, const movein_fn &movein,
                  bool &done);

/*
 * Count rows put into table oid's delta store; at rg_max_rows the
 * table is queued for its seal (sealer.cc). A no-op with no sealer.
 */
void sealer_count(const catalog_t *cat, uint64_t oid, uint64_t rows);

/*
 * Count delete markers committed on the row group of file set fsid of
 * table oid; at a fifth of a row group's rows, the row group is
 * queued for its compaction (sealer.cc). A no-op with no sealer.
 */
void sealer_count_marks(const catalog_t *cat, uint64_t oid, uint64_t fsid,
                        uint64_t marks);

/* Queue the row group of file set fsid of table oid for its compaction. */
void sealer_enqueue_rg(const catalog_t *cat, uint64_t oid, uint64_t fsid);

/* Queue file set fsid of table oid for its fold (../COLSTORE2.md §6.6). */
void sealer_enqueue_fold(const catalog_t *cat, uint64_t oid, uint64_t fsid);

/* Queue table oid for its seal; a no-op with no sealer. */
void sealer_enqueue(const catalog_t *cat, uint64_t oid);

} // namespace sm3

#endif /* SM3_COLSTORE_H */
