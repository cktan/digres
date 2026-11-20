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

/* The relids of a column store table's keys (../COLSTORE.md §4.3). */
constexpr uint16_t marker_relid = 2;
constexpr uint16_t filter_relid = 4;

/* The rows of a row group (../COLSTORE.md §7). */
constexpr uint32_t rg_max_rows = 10000;

/* The name of the _id CG's file, among the columns' attnums. */
constexpr const char *rg_id_name = "id";

/* One CG of a row group, as its RG key records it (§4.4). */
struct rg_col_t {
  int32_t attnum = 0;     /* 0-based; -1 for the _id CG */
  uint64_t size = 0;      /* the CG file's bytes */
  uint32_t crc = 0;       /* its CRC32C */
  uint64_t long_size = 0; /* its long-value file's, 0 for none */
  uint32_t long_crc = 0;
  sm2::cg_stats_t stats; /* its zonemap */
};

/* A row group's metadata: the value of its RG key. */
struct rg_meta_t {
  uint32_t rows = 0;
  uint64_t id_min = 0, id_max = 0;
  std::vector<rg_col_t> cols; /* the _id CG first */
};

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
 * its rg_id, write its CG files, put its filter keys and its RG key.
 * Nothing to write is no error. Returns 0 or -1.
 */
int rg_build_flush(rg_build_t &b);

/*
 * A row group read for a scan: its metadata, the CGs of the columns
 * the scan reads, the _ids its delete markers name, and the row it
 * is at.
 */
struct rg_read_t {
  uint64_t rg_id = 0;
  rg_meta_t meta;
  std::vector<sm2::cg_t *> cgs; /* by column; nullptr: not read */
  sm2::cg_t *ids = nullptr;
  std::set<uint64_t> deleted;
  uint32_t at = 0;
  std::vector<std::string> longs; /* long values of the row at hand */
};

/* Free what r holds. */
void rg_read_close(rg_read_t &r);

/*
 * Read row group rg_id, with metadata meta, for a scan of table:
 * the CGs of the columns in cols, every column if cols is empty,
 * and its delete markers at the transaction's view. Returns 0 or -1.
 */
int rg_read_open(table_t *table, uint64_t rg_id, rg_meta_t meta,
                 const std::vector<bool> &cols, rg_read_t &r);

/*
 * The next live row of r, as datums into values and isnull, one per
 * column of the table; a column not read is null. 0 with id set, 1
 * at the end, or -1. The datums view r, until its next call.
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
  std::vector<std::pair<uint64_t, std::string>> rgs; /* (rg_id, key value) */
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

/* The rg_id of the row group c's last row came from. */
inline uint64_t col_scan_rg(const col_scan_t *c) { return c->rg.rg_id; }

/*
 * The _ids of row group rg_id's delete markers that the transaction
 * sees, into ids: its rows deleted (§4.3).
 */
int col_markers(table_t *table, uint64_t rg_id, std::set<uint64_t> &ids);

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

/* The key of row id's delete marker in row group rg_id (§4.3). */
std::string marker_key(uint64_t rg_id, uint64_t id);

/*
 * The row group that holds row id's live copy at the newest
 * committed state, not the transaction's snapshot: a writer follows
 * a row the seal moved (../COLSTORE.md §10.2). 0 with rg_id set, 1
 * if no row group does, or -1.
 */
int col_find_latest(table_t *table, uint64_t id, uint64_t &rg_id);

/*
 * Compact row groups rgs of table into new ones (../COLSTORE.md §11),
 * at the newest committed state: each live row locked if free now,
 * marked moved in its old row group, and added to the new; then the
 * old row groups' keys erased. A row with a writer in flight or
 * prepared makes it give up, done false, with nothing left written.
 * Returns 0 or -1.
 */
int col_compact(table_t *table, std::span<const uint64_t> rgs, bool &done);

/*
 * Count rows put into table oid's delta store; at rg_max_rows the
 * table is queued for its seal (sealer.cc). A no-op with no sealer.
 */
void sealer_count(const catalog_t *cat, uint64_t oid, uint64_t rows);

/*
 * Count delete markers committed on row group rg_id of table oid; at
 * a fifth of a row group's rows, the row group is queued for its
 * compaction (sealer.cc). A no-op with no sealer.
 */
void sealer_count_marks(const catalog_t *cat, uint64_t oid, uint64_t rg_id,
                        uint64_t marks);

/* Queue row group rg_id of table oid for its compaction. */
void sealer_enqueue_rg(const catalog_t *cat, uint64_t oid, uint64_t rg_id);

/* Queue table oid for its seal; a no-op with no sealer. */
void sealer_enqueue(const catalog_t *cat, uint64_t oid);

} // namespace sm3

#endif /* SM3_COLSTORE_H */
