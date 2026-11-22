/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_colstore.cc -- column store tables (../COLSTORE.md): their
 * kind in the catalog, what they refuse, and their data.
 */

#include "../src/colstore/colstore.h"
#include "analyze.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

/*
 * A catalog on a fresh database, loaded, with a session ss over s; its
 * row groups of rg_rows rows, if not 0 (sm3::cat_set_rg_rows); with
 * toast, its long datums out of line (sm3::cat_set_toast).
 */
class col_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  explicit col_db_t(uint32_t rows = 0, bool toast = false)
      : rg_rows(rows), toast(toast) {
    s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
  }

  /* Close the session. */
  ~col_db_t() { sm3::session_close(ss); }

  /* Restart, then load the catalog and open a session again. */
  sm3::catalog_t *reload() {
    sm3::session_close(ss);
    ss = nullptr;
    cat = nullptr;
    restart();
    s = open();
    load();
    return cat;
  }

  /* Begin a transaction on ss at level. */
  void begin(sm1::xctlevel_t level = sm1::xctlevel_t::read_committed) {
    REQUIRE(sm3::begin(ss, level) == 0);
  }

  /* Commit ss's transaction. */
  void commit() { REQUIRE_MESSAGE(sm3::commit(ss) == 0, sm1::errmsg()); }

  sm1::session_t *s = nullptr;
  sm3::session_t *ss = nullptr;
  sm3::catalog_t *cat = nullptr;
  const uint32_t rg_rows;
  const bool toast;

private:
  /* Load the catalog, with its row group size; open ss over s. */
  void load() {
    REQUIRE_MESSAGE(sm3::cat_open(env(), "db", cat) == 0, sm1::errmsg());
    if (rg_rows != 0) {
      REQUIRE(sm3::cat_set_rg_rows(cat, rg_rows) == 0);
    }
    if (toast) {
      REQUIRE(sm3::cat_set_toast(cat, true) == 0);
    }
    REQUIRE(sm3::session_open(cat, s, ss) == 0);
  }
};

/* Make the table name (a int4, b text) in public; its oid. */
uint64_t make(col_db_t &db, const char *name, bool column_store) {
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"b", sm2::type_text.code, -1, false}};
  uint64_t oid = 0;
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, name, cols, oid,
                            column_store) == 0);
  return oid;
}

} // namespace

// Verify a table's kind is recorded, and lasts a restart.
// 1. Make a column store table c and a row store table r.
// 2. After a restart, c is a column store table, r is not, and c's
//    row group sequence gives ids.
TEST_CASE("a column store table keeps its kind across a restart") {
  col_db_t db;
  // 1. The tables.
  db.begin();
  uint64_t c = make(db, "c", true);
  uint64_t r = make(db, "r", false);
  db.commit();
  // 2. After a restart.
  sm3::catalog_t *cat = db.reload();
  auto dc = sm3::cat_get_table(cat, c);
  auto dr = sm3::cat_get_table(cat, r);
  REQUIRE(dc != nullptr);
  REQUIRE(dr != nullptr);
  CHECK(dc->column_store);
  CHECK(dc->rec.column_store);
  CHECK_FALSE(dr->column_store);
  REQUIRE(sm1::begin(db.s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t id = 0;
  CHECK(sm1::nextseq(db.s, "sm3.rg." + std::to_string(c), id) == 0);
  CHECK(id == 1);
  CHECK(sm1::nextseq(db.s, "sm3.rg." + std::to_string(r), id) == -1);
  REQUIRE(sm1::commit(db.s) == 0);
}

// Verify a column store table refuses an index, and a row store one
// does not.
// 1. An index on c fails with SM_ERR_INVALID.
// 2. An index on r is made.
TEST_CASE("a column store table refuses an index") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  uint64_t r = make(db, "r", false);
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  uint64_t ix = 0;
  // 1. Refused.
  CHECK(sm3::create_index(db.ss, c, "c_a", keys, {}, false, ix) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  // 2. Made.
  CHECK_MESSAGE(sm3::create_index(db.ss, r, "r_a", keys, {}, false, ix) == 0,
                sm1::errmsg());
  db.commit();
}

// Verify a SERIALIZABLE transaction cannot open a column store table,
// and the other levels can.
// 1. At READ COMMITTED and REPEATABLE READ, c opens.
// 2. At SERIALIZABLE, c's open fails with SM_ERR_UNSUPPORTED; r
//    opens.
TEST_CASE("a SERIALIZABLE transaction cannot use a column store table") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  uint64_t r = make(db, "r", false);
  db.commit();
  sm3::table_t *t = nullptr;
  // 1. The other levels.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read}) {
    db.begin(level);
    REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
    sm3::tbl_close(t);
    db.commit();
  }
  // 2. SERIALIZABLE.
  db.begin(sm1::xctlevel_t::serializable);
  CHECK(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UNSUPPORTED);
  REQUIRE(sm3::tbl_open(db.ss, r, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::tbl_close(t);
  db.commit();
}

// Verify a dropped column store table takes its row group sequence.
// 1. Make c, and drop it.
// 2. Its row group sequence is gone.
TEST_CASE("a dropped column store table takes its sequence") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. Drop it.
  db.begin();
  REQUIRE(sm3::drop_table(db.ss, c) == 0);
  db.commit();
  // 2. The sequence is gone.
  REQUIRE(sm1::begin(db.s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t id = 0;
  CHECK(sm1::nextseq(db.s, "sm3.rg." + std::to_string(c), id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(db.s) == 0);
}

namespace {

/* Load n rows (a = i, b = "n" + i % 7) into c, in one transaction. */
void load_rows(col_db_t &db, uint64_t c, int n, bool commit = true) {
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_load_open(t, l) == 0, sm1::errmsg());
  for (int i = 0; i < n; i++) {
    std::string b = sm2::make_varlena("n" + std::to_string(i % 7));
    sm2::datum_t d[2] = {sm2::datum_from_int32(i),
                         reinterpret_cast<sm2::datum_t>(b.data())};
    bool null[2] = {false, false};
    uint64_t id = 0;
    REQUIRE_MESSAGE(sm3::tbl_load_add(l, d, null, id) == 0, sm1::errmsg());
  }
  REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  if (commit) {
    db.commit();
  } else {
    REQUIRE(sm3::abort(db.ss) == 0);
  }
}

/* What a scan of c gives: the a of each row, and its b, or "-". */
std::map<int32_t, std::string> scan_rows(col_db_t &db, uint64_t c,
                                         std::vector<bool> cols = {},
                                         std::vector<sm3::col_bound_t> b = {}) {
  std::map<int32_t, std::string> out;
  db.begin(sm1::xctlevel_t::repeatable_read);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_scan_columns(t, cols, b, 0, UINT64_MAX, scan) == 0,
                  sm1::errmsg());
  uint64_t id;
  std::string_view tuple;
  const sm3::table_def_t &def = sm3::tbl_def(t);
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(def.desc, tuple, d, null) == 0);
    int32_t a = null[0] ? -1 : sm2::datum_to_int32(d[0]);
    std::string v = "-";
    if (!null[1]) {
      std::string_view bytes;
      REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], bytes) == 0);
      v = std::string(sm2::varlena_payload(bytes));
    }
    CHECK(out.count(a) == 0);
    out[a] = v;
  }
  CHECK(rc == 1);
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  db.commit();
  return out;
}

/* The row groups of c's store its transaction sees. */
size_t row_groups(col_db_t &db, uint64_t c) {
  db.begin();
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(db.cat, c);
  std::vector<std::pair<uint64_t, std::string>> sets;
  REQUIRE(sm1::fileset_list(db.s, def->storeid, sets) == 0);
  db.commit();
  return sets.size();
}

} // namespace

// Verify a bulk load writes whole row groups, the rest to the delta
// store, and a scan reads every row once, before and after a restart.
// 1. Load 25,000 rows into c: two row groups, 5,000 rows in the delta.
// 2. A scan gives each row once, with its values.
// 3. After a restart, the same.
TEST_CASE("a bulk load writes row groups, and a scan reads every row") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The load.
  load_rows(db, c, 25000);
  CHECK(row_groups(db, c) == 2);
  // 2. Every row, once.
  auto rows = scan_rows(db, c);
  REQUIRE(rows.size() == 25000);
  CHECK(rows[0] == "n0");
  CHECK(rows[12345] == "n" + std::to_string(12345 % 7));
  CHECK(rows[24999] == "n" + std::to_string(24999 % 7));
  // 3. After a restart.
  db.reload();
  CHECK(scan_rows(db, c).size() == 25000);
}

// Verify a scan's bounds skip row groups, and its columns are those
// read.
// 1. Load 25,000 rows, a ascending: row group 1 holds a 0..9,999,
//    row group 2 a 10,000..19,999.
// 2. With a = 15,000, row group 1 is skipped: the scan gives row
//    group 2's rows and the delta store's, 15,000.
// 3. With a > 30,000, both are skipped: the delta's 5,000 alone.
// 4. Reading a alone, b is null in the row groups' rows.
TEST_CASE("a scan's bounds skip row groups that cannot match") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 25000);
  auto key = [](int32_t v) {
    std::string k, d(4, '\0');
    memcpy(d.data(), &v, 4);
    REQUIRE(sm2::type_int4.key_encode(d, -1, nullptr, k) == 0);
    return k;
  };
  // 2. An equality.
  sm3::col_bound_t eq{0, {}};
  eq.bound.has_lo = eq.bound.has_hi = true;
  eq.bound.lo = eq.bound.hi = key(15000);
  CHECK(scan_rows(db, c, {}, {eq}).size() == 15000);
  // 3. A range past every row group.
  sm3::col_bound_t gt{0, {}};
  gt.bound.has_lo = true;
  gt.bound.lo_incl = false;
  gt.bound.lo = key(30000);
  CHECK(scan_rows(db, c, {}, {gt}).size() == 5000);
  // 4. One column read.
  auto rows = scan_rows(db, c, {true, false});
  REQUIRE(rows.size() == 25000);
  CHECK(rows[5] == "-");
  CHECK(rows[24000] == "n" + std::to_string(24000 % 7));
}

// Verify an aborted bulk load leaves no row group and no row.
// 1. Load 12,000 rows, and abort.
// 2. No row group is seen, and a scan gives no row.
TEST_CASE("an aborted bulk load leaves no row group") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The aborted load.
  load_rows(db, c, 12000, false);
  // 2. Nothing.
  CHECK(row_groups(db, c) == 0);
  CHECK(scan_rows(db, c).empty());
}

namespace {

/* Where row a is: (place, _id), read by a scan of c. */
std::pair<sm3::place_t, uint64_t> where(col_db_t &db, uint64_t c, int32_t a) {
  std::pair<sm3::place_t, uint64_t> at{{UINT64_MAX, 0}, 0};
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    if (!null[0] && sm2::datum_to_int32(d[0]) == a) {
      at = {sm3::tbl_scan_place(scan), id};
    }
  }
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  db.commit();
  return at;
}

/* The row (a, b) as datums, b's varlena in buf. */
struct row_t {
  row_t(int32_t a, const std::string &b) : buf(sm2::make_varlena(b)) {
    d[0] = sm2::datum_from_int32(a);
    d[1] = reinterpret_cast<sm2::datum_t>(buf.data());
  }
  std::string buf;
  sm2::datum_t d[2];
  bool null[2] = {false, false};
};

} // namespace

// Verify a row of a row group is deleted and updated by its name.
// 1. Load 15,000 rows: a = 7 and a = 8 in the row group, a = 12,000
//    in the delta store.
// 2. Delete a = 7 at (rg_id, _id): a scan no longer gives it; a
//    second delete fails with SM_ERR_NOTFOUND.
// 3. Update a = 8 to b = "x": a scan gives it once, from the delta
//    store, under its _id.
// 4. Delete a = 12,000 in the delta store, at (0, _id).
TEST_CASE("a row of a row group is deleted and updated") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 15000);
  auto [rg7, id7] = where(db, c, 7);
  auto [rg8, id8] = where(db, c, 8);
  auto [rg12, id12] = where(db, c, 12000);
  CHECK(rg7.fsid != 0);
  CHECK(rg8.fsid == rg7.fsid);
  CHECK(rg12.fsid == 0);
  // 2. Delete a = 7.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t, rg7, id7) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 14999);
  CHECK(rows.count(7) == 0);
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  CHECK(sm3::tbl_delete_at(t, rg7, id7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(db.ss) == 0);
  // 3. Update a = 8.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r8(8, "x");
  REQUIRE_MESSAGE(sm3::tbl_update_at(t, rg8, id8, r8.d, r8.null) == 0,
                  sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  rows = scan_rows(db, c);
  CHECK(rows.size() == 14999);
  CHECK(rows[8] == "x");
  auto [rg8b, id8b] = where(db, c, 8);
  CHECK(rg8b.fsid == 0);
  CHECK(id8b == id8);
  // 4. Delete in the delta store.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::tbl_delete_at(t, {}, id12) == 0);
  sm3::tbl_close(t);
  db.commit();
  CHECK(scan_rows(db, c).count(12000) == 0);
}

// Verify concurrent writers of a row of a row group, by level.
// 1. R (REPEATABLE READ) begins; S deletes a = 9 and commits; R's
//    delete of it fails with SM_ERR_CONFLICT.
// 2. S updates a = 10 into the delta store, and commits; C (READ
//    COMMITTED, its statement's snapshot older) deletes it at its
//    row group's name: SM_ERR_UPDATED, then at (0, _id): done.
// 3. An aborted delete leaves the row.
TEST_CASE("concurrent writers of a row of a row group") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  load_rows(db, c, 12000);
  auto [rg9, id9] = where(db, c, 9);
  auto [rg10, id10] = where(db, c, 10);
  auto [rg11, id11] = where(db, c, 11);
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  sm3::table_t *t = nullptr, *t2 = nullptr;
  // 1. R, then S's delete.
  db.begin(sm1::xctlevel_t::repeatable_read);
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE(sm3::tbl_delete_at(t2, rg9, id9) == 0);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  CHECK(sm3::tbl_delete_at(t, rg9, id9) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm3::tbl_close(t);
  sm3::abort(db.ss);
  // 2. S's update, then C's delete.
  db.begin(sm1::xctlevel_t::read_committed);
  REQUIRE(sm1::stmt_begin(db.s) == 0);
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  std::string_view tuple;
  // A read takes the statement's snapshot, before S commits.
  CHECK(sm3::tbl_read(t, id11, tuple) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  row_t r10(10, "y");
  REQUIRE(sm3::tbl_update_at(t2, rg10, id10, r10.d, r10.null) == 0);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  CHECK(sm3::tbl_delete_at(t, rg10, id10) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t, {}, id10) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  CHECK(scan_rows(db, c).count(10) == 0);
  // 3. An aborted delete.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::tbl_delete_at(t, rg11, id11) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(db.ss) == 0);
  CHECK(scan_rows(db, c).count(11) == 1);
  sm3::session_close(ss2);
}

namespace {

/* Insert rows a = from .. to - 1 into c, one at a time, in one transaction. */
void insert_rows(col_db_t &db, uint64_t c, int from, int to) {
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  for (int i = from; i < to; i++) {
    row_t r(i, "n" + std::to_string(i % 7));
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d, r.null, id) == 0);
  }
  sm3::tbl_close(t);
  db.commit();
}

/* Seal c once, in a transaction of its own; whether it sealed. */
bool seal(sm3::session_t *ss, uint64_t c) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool sealed = false;
  REQUIRE_MESSAGE(sm3::tbl_seal(t, sealed) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return sealed;
}

} // namespace

// Verify the seal moves the delta store's first rows into a row group,
// and seals no partial one.
// 1. Insert 12,000 rows one at a time: all in the delta store.
// 2. Seal: one row group of 10,000; a scan gives every row once.
// 3. Seal again: 2,000 rows are too few, nothing changes.
// 4. After a restart, every row once still.
TEST_CASE("the seal moves rows into a row group, never a partial one") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  insert_rows(db, c, 0, 12000);
  CHECK(row_groups(db, c) == 0);
  // 2. The seal.
  CHECK(seal(db.ss, c));
  CHECK(row_groups(db, c) == 1);
  auto rows = scan_rows(db, c);
  REQUIRE(rows.size() == 12000);
  CHECK(rows[11999] == "n" + std::to_string(11999 % 7));
  CHECK(where(db, c, 5).first.fsid != 0);
  CHECK(where(db, c, 11000).first.fsid == 0);
  // 3. Too few rows.
  CHECK_FALSE(seal(db.ss, c));
  CHECK(row_groups(db, c) == 1);
  CHECK(scan_rows(db, c).size() == 12000);
  // 4. After a restart.
  db.reload();
  CHECK(scan_rows(db, c).size() == 12000);
}

// Verify the seal passes over a locked row, and a writer whose
// snapshot is older than the seal follows the row it moved.
// 1. Insert 10,001 rows; S2 locks a = 3 FOR UPDATE.
// 2. Seal: 10,000 rows sealed, a = 3 left in the delta store.
// 3. R (REPEATABLE READ) began before a second seal of rows 10,001 to
//    20,000; it updates a = 15,000 by its delta name (0, _id): it
//    follows the row to its row group, and commits.
// 4. A scan gives a = 15,000 once, with R's value.
TEST_CASE("the seal passes over a locked row; a writer follows a sealed row") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows, and a lock on one.
  insert_rows(db, c, 0, 10001);
  auto [rg3, id3] = where(db, c, 3);
  CHECK(rg3.fsid == 0);
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE(sm3::tbl_lock(t2, id3, sm1::SM_LOCKMODE_EX) == 0);
  // 2. The seal passes it over.
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  CHECK(seal(ss3, c));
  CHECK(where(db, c, 3).first.fsid == 0);
  CHECK(where(db, c, 4).first.fsid != 0);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 3. R, then a second seal, then R's update.
  insert_rows(db, c, 10001, 20001);
  auto [rgx, idx] = where(db, c, 15000);
  CHECK(rgx.fsid == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  std::string_view tuple;
  REQUIRE(sm3::tbl_read(t2, idx, tuple) == 0);
  CHECK(seal(ss3, c));
  CHECK(where(db, c, 15000).first.fsid != 0);
  row_t r(15000, "r");
  REQUIRE_MESSAGE(sm3::tbl_update_at(t2, {}, idx, r.d, r.null) == 0,
                  sm1::errmsg());
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 4. Once, with R's value.
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 20001);
  CHECK(rows[15000] == "r");
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify the seal's job seals a delta store once a transaction's
// commit makes it a row group's worth, and not before.
// 1. Attach a sealer to the catalog.
// 2. Commit 9,999 inserts: nothing is queued; no row group.
// 3. Commit one more: the table is queued and sealed.
// 4. A scan gives every row once.
TEST_CASE("the seal's job seals a delta store at a row group's worth") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The sealer.
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  // 2. Not yet.
  insert_rows(db, c, 0, 9999);
  sm3::sealer_wait_idle(db.cat);
  CHECK(row_groups(db, c) == 0);
  // 3. Now.
  insert_rows(db, c, 9999, 10000);
  sm3::sealer_wait_idle(db.cat);
  CHECK(row_groups(db, c) == 1);
  // 4. Every row once.
  CHECK(scan_rows(db, c).size() == 10000);
  sm3::sealer_detach(db.cat);
}

namespace {

/* Compact file set fsid of c in a transaction of its own; done. */
bool compact(sm3::session_t *ss, uint64_t c, uint64_t fsid) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool done = false;
  REQUIRE_MESSAGE(sm3::tbl_compact_rg(t, fsid, done) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return done;
}

/* Eliminate file set fsid of c in a transaction of its own; done. */
bool eliminate(sm3::session_t *ss, uint64_t c, uint64_t fsid) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool done = false;
  REQUIRE_MESSAGE(sm3::tbl_eliminate_rg(t, fsid, done) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return done;
}

/* Delete rows a = from .. to - 1 of c by their names. */
void delete_rows(col_db_t &db, uint64_t c, int from, int to) {
  std::vector<std::pair<sm3::place_t, uint64_t>> names;
  for (int a = from; a < to; a++) {
    names.push_back(where(db, c, a));
  }
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  for (auto [rg, id] : names) {
    REQUIRE(sm3::tbl_delete_at(t, rg, id) == 0);
  }
  sm3::tbl_close(t);
  db.commit();
}

} // namespace

// Verify RG compaction rewrites a row group's live rows into its next
// version, and gives up on a writer of the row group in flight.
// 1. Load 10,000 rows: one row group; delete a = 0 .. 29.
// 2. S2 deletes a = 50, in flight: compaction gives up, and nothing
//    changes.
// 3. S2 commits: compaction makes the next version, of 9,969 rows,
//    and the old one is gone; a scan gives 9,969 rows, without
//    a = 50.
TEST_CASE("RG compaction rewrites live rows, and gives up on a writer") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows, some deleted.
  load_rows(db, c, 10000);
  uint64_t rg = where(db, c, 100).first.fsid;
  REQUIRE(rg != 0);
  delete_rows(db, c, 0, 30);
  // 2. A writer in flight.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  auto [p50, id50] = where(db, c, 50);
  REQUIRE(sm3::tbl_delete_at(t2, p50, id50) == 0);
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  CHECK_FALSE(compact(ss3, c, {rg}));
  CHECK(where(db, c, 100).first.fsid == rg);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 3. Now.
  CHECK(compact(ss3, c, {rg}));
  CHECK(row_groups(db, c) == 1);
  uint64_t rg2 = where(db, c, 100).first.fsid;
  CHECK(rg2 != 0);
  CHECK(rg2 != rg);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 9969);
  CHECK(rows.count(50) == 0);
  CHECK(rows.count(51) == 1);
  CHECK(rows.count(10) == 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify a writer whose snapshot is older than RG compaction follows
// a moved row to its new row group.
// 1. Load 10,000 rows; R (REPEATABLE READ) reads; RG compaction runs.
// 2. R deletes a = 7 at its old name: it follows the moved marker to
//    the new row group, and commits.
// 3. A scan gives 9,999 rows, and no a = 7.
TEST_CASE("a writer follows a row RG compaction moved") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  load_rows(db, c, 10000);
  auto [rg7, id7] = where(db, c, 7);
  // 1. R, then RG compaction.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  std::string_view tuple;
  CHECK(sm3::tbl_read(t2, id7, tuple) == 0);
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  CHECK(compact(ss3, c, {rg7.fsid}));
  // 2. R's delete at the old name.
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t2, rg7, id7) == 0, sm1::errmsg());
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 3. Gone.
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 9999);
  CHECK(rows.count(7) == 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify the sealer compacts a row group that committed delete
// markers have made a fifth dead, with row groups of 100 rows.
// 1. Row groups of 0 rows, or of more than 10k, are refused. Attach a
//    sealer; load 100 rows: one row group.
// 2. Delete 19 of its rows: not yet a fifth; nothing is queued.
// 3. Delete one more: the row group is queued and compacted into a
//    new one of 80 rows.
TEST_CASE("the sealer compacts a row group a fifth dead") {
  col_db_t db(100);
  CHECK(sm3::cat_set_rg_rows(db.cat, 0) == -1);
  CHECK(sm3::cat_set_rg_rows(db.cat, 10001) == -1);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The sealer, and the rows.
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  load_rows(db, c, 100);
  uint64_t rg = where(db, c, 50).first.fsid;
  REQUIRE(rg != 0);
  REQUIRE(row_groups(db, c) == 1);
  // 2. Not yet.
  delete_rows(db, c, 0, 19);
  sm3::sealer_wait_idle(db.cat);
  CHECK(where(db, c, 50).first.fsid == rg);
  // 3. Now.
  delete_rows(db, c, 19, 20);
  sm3::sealer_wait_idle(db.cat);
  uint64_t rg2 = where(db, c, 50).first.fsid;
  CHECK(rg2 != 0);
  CHECK(rg2 != rg);
  CHECK(row_groups(db, c) == 1);
  CHECK(scan_rows(db, c).size() == 80);
  sm3::sealer_detach(db.cat);
}

namespace {

/* Row tuple's a and b, b "-" if null. */
std::pair<int32_t, std::string> decode(sm3::table_t *t,
                                       std::string_view tuple) {
  sm2::datum_t d[2];
  bool null[2];
  REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
  std::string v = "-";
  if (!null[1]) {
    std::string_view bytes;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], bytes) == 0);
    v = std::string(sm2::varlena_payload(bytes));
  }
  return {null[0] ? -1 : sm2::datum_to_int32(d[0]), v};
}

} // namespace

// Verify a read and a write of a column store table's row by its _id
// alone find the row in its row group, as digres names a row it has
// not just scanned (QUESTION.md 3).
// 1. Load 15,000 rows: a row group of a 0..9,999, and the delta store.
// 2. Read a = 5 by its _id: as the snapshot sees it, the newest
//    under its lock, and into a pin.
// 3. Update a = 5 by its _id: the new version is in the delta store.
// 4. Delete a = 6 by its _id, with the row as it was handed back; a
//    second delete finds no row.
// 5. After RG compaction moves a = 8, read and delete it by its _id.
TEST_CASE("a read and a write by _id alone find a row of a row group") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 15000);
  auto [rg5, id5] = where(db, c, 5);
  auto [rg6, id6] = where(db, c, 6);
  REQUIRE(rg5.fsid != 0);
  REQUIRE(rg6.fsid == rg5.fsid);
  // 2. The reads, at READ COMMITTED: a newest read needs it.
  db.begin(sm1::xctlevel_t::read_committed);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  std::string_view tuple;
  REQUIRE_MESSAGE(sm3::tbl_read(t, id5, tuple) == 0, sm1::errmsg());
  CHECK(decode(t, tuple) == std::pair<int32_t, std::string>(5, "n5"));
  // A newest read is under the row's lock, as after a write since.
  REQUIRE(sm3::tbl_lock(t, id5, sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm3::tbl_lock(t, id6, sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE_MESSAGE(sm3::tbl_read_latest(t, id5, tuple) == 0,
                  std::string(sm1::errmsg()));
  CHECK(decode(t, tuple).first == 5);
  sm3::pin_ref_t ref{};
  REQUIRE(sm3::tbl_read(t, id5, ref, tuple) == 0);
  CHECK(decode(t, tuple).first == 5);
  REQUIRE(sm3::tbl_read_latest(t, id6, ref, tuple) == 0);
  CHECK(decode(t, tuple).first == 6);
  // 3. The update.
  row_t r5(5, "five");
  REQUIRE_MESSAGE(sm3::tbl_update(t, id5, r5.d, r5.null) == 0, sm1::errmsg());
  // 4. The delete, and again.
  std::string_view old;
  REQUIRE_MESSAGE(sm3::tbl_delete(t, id6, ref, old) == 0, sm1::errmsg());
  CHECK(decode(t, old) == std::pair<int32_t, std::string>(6, "n6"));
  CHECK(sm3::tbl_delete(t, id6) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::tbl_read(t, id6, tuple) == 1);
  sm3::unpin(ref);
  sm3::tbl_close(t);
  db.commit();
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 14999);
  CHECK(rows[5] == "five");
  CHECK(rows.count(6) == 0);
  CHECK(where(db, c, 5).first.fsid == 0);
  // 5. A row RG compaction moved.
  auto [rg8, id8] = where(db, c, 8);
  REQUIRE(compact(db.ss, c, {rg8.fsid}));
  CHECK(where(db, c, 8).first.fsid != rg8.fsid);
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::tbl_read(t, id8, tuple) == 0);
  CHECK(decode(t, tuple).first == 8);
  REQUIRE_MESSAGE(sm3::tbl_delete(t, id8) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  CHECK(scan_rows(db, c).count(8) == 0);
}

// Verify ANALYZE's sample of a column store table: an exact row
// count, rows from the row groups and the delta store by their
// shares, no deleted row, and the CGs' bytes in the table's size.
// 1. Load 25,000 rows: two row groups, 5,000 in the delta store.
// 2. Delete 10 rows of the first row group by name; insert 7 rows
//    into the delta store.
// 3. Sample at target 1, 300 rows: 24,997 live, exactly; 60 rows
//    from the delta store (300 x 5,007 / 24,997), 241 from the row
//    groups (120 and 121, by their live rows); none deleted.
// 4. The table's size counts the CGs' bytes on disk.
TEST_CASE("ANALYZE samples a column store table's row groups and delta") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The load.
  load_rows(db, c, 25000);
  REQUIRE(row_groups(db, c) == 2);
  // 2. The deletes and the inserts.
  std::set<uint64_t> gone;
  for (int32_t a = 0; a < 10; a++) {
    auto [rg, id] = where(db, c, a);
    REQUIRE(rg.fsid != 0);
    db.begin();
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
    REQUIRE_MESSAGE(sm3::tbl_delete_at(t, rg, id) == 0, sm1::errmsg());
    sm3::tbl_close(t);
    db.commit();
    gone.insert(id);
  }
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  for (int32_t a = 30000; a < 30007; a++) {
    row_t r(a, "new");
    uint64_t id = 0;
    REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d, r.null, id) == 0, sm1::errmsg());
  }
  sm3::tbl_close(t);
  db.commit();
  // 3. The sample.
  db.begin(sm1::xctlevel_t::repeatable_read);
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  std::mt19937_64 rng(42);
  sm3::sample_t s;
  REQUIRE_MESSAGE(sm3::sample_table(t, 1, rng, s) == 0, sm1::errmsg());
  CHECK(s.live_rows == 24997);
  CHECK(s.rows_read == 24997);
  CHECK(s.runs == 2);
  CHECK(s.rows.size() == 301);
  size_t from_delta = 0;
  for (size_t i = 0; i < s.rows.size(); i++) {
    CHECK(gone.count(s.rows[i].id) == 0);
    if (s.rows[i].id > 20000) {
      from_delta++;
    }
    if (i > 0) {
      CHECK(s.rows[i - 1].id < s.rows[i].id);
    }
  }
  CHECK(from_delta == 60);
  // 4. The size.
  uint64_t disk = 0, mem = 0;
  REQUIRE(sm3::tbl_size(t, disk, mem) == 0);
  CHECK(disk > 0);
  sm3::tbl_close(t);
  db.commit();
}

namespace {

/*
 * Each row of c by its a: column col's integer value, -1 if null; a
 * and col int4 or int8.
 */
std::map<int64_t, int64_t> scan_col(col_db_t &db, uint64_t c, size_t col) {
  std::map<int64_t, int64_t> out;
  db.begin(sm1::xctlevel_t::repeatable_read);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  const sm3::table_def_t &def = sm3::tbl_def(t);
  auto value = [&](std::string_view tuple, size_t i) -> int64_t {
    sm2::datum_t d;
    bool null;
    REQUIRE(sm2::tup_decode(def.desc, tuple, i, d, null) == 0);
    if (null) {
      return -1;
    }
    return def.columns[i].type == &sm2::type_int8 ? sm2::datum_to_int64(d)
                                                  : sm2::datum_to_int32(d);
  };
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    out[value(tuple, 0)] = value(tuple, col);
  }
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  db.commit();
  return out;
}

/* The rg_ids of c's row groups its transaction sees. */
std::vector<uint64_t> rg_ids(col_db_t &db, uint64_t c) {
  std::vector<uint64_t> out;
  db.begin();
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(db.cat, c);
  std::vector<std::pair<uint64_t, std::string>> sets;
  REQUIRE(sm1::fileset_list(db.s, def->storeid, sets) == 0);
  for (const auto &[rg, value] : sets) {
    out.push_back(rg);
  }
  db.commit();
  return out;
}

/* The attnums of the CGs of c's row groups, the _id CG's left out. */
std::set<int32_t> cg_attnums(col_db_t &db, uint64_t c) {
  std::set<int32_t> out;
  db.begin();
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(db.cat, c);
  std::vector<std::pair<uint64_t, std::string>> sets;
  REQUIRE(sm1::fileset_list(db.s, def->storeid, sets) == 0);
  for (const auto &[rg, value] : sets) {
    sm3::rg_meta_t meta;
    REQUIRE(sm3::rg_meta_decode(value, meta) == 0);
    for (const sm3::rg_col_t &col : meta.cols) {
      if (col.attnum >= 0) {
        out.insert(col.attnum);
      }
    }
  }
  db.commit();
  return out;
}

} // namespace

// Verify a column store table's DDL (../COLSTORE.md §14): ADD COLUMN
// with no rewrite, DROP COLUMN, a rewrite, and TRUNCATE.
// 1. Load 15,000 rows: a row group, and 5,000 in the delta store.
// 2. ADD COLUMN z with a missing value of 42: every row reads 42,
//    the row group's from no CG. A NOT NULL column with no missing
//    value is refused, as the row group holds rows.
// 3. DROP COLUMN b: every row reads b as null, and RG compaction
//    leaves b's CG out of the new row group, z's in.
// 4. ALTER COLUMN a TYPE int8, a rewrite: the new store has a row
//    group, as a bulk load would make, and every a is converted.
// 5. TRUNCATE: no row and no row group.
TEST_CASE("a column store table's DDL") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 15000);
  REQUIRE(row_groups(db, c) == 1);
  // 2. ADD COLUMN.
  int32_t v42 = 42;
  std::string missing(reinterpret_cast<const char *>(&v42), 4);
  db.begin();
  REQUIRE_MESSAGE(sm3::add_column(db.ss, c,
                                  {"z", sm2::type_int4.code, -1, false},
                                  std::string_view(missing)) == 0,
                  sm1::errmsg());
  db.commit();
  auto z = scan_col(db, c, 2);
  REQUIRE(z.size() == 15000);
  CHECK(z[0] == 42);
  CHECK(z[9999] == 42);
  CHECK(z[14999] == 42);
  db.begin();
  CHECK(sm3::add_column(db.ss, c, {"w", sm2::type_int4.code, -1, true},
                        std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
  REQUIRE(sm3::abort(db.ss) == 0);
  // 3. DROP COLUMN.
  db.begin();
  REQUIRE_MESSAGE(sm3::drop_column(db.ss, c, "b") == 0, sm1::errmsg());
  db.commit();
  CHECK(cg_attnums(db, c) == std::set<int32_t>{0, 1});
  for (uint64_t fsid : rg_ids(db, c)) {
    REQUIRE(compact(db.ss, c, fsid));
  }
  CHECK(cg_attnums(db, c) == std::set<int32_t>{0, 2});
  z = scan_col(db, c, 2);
  CHECK(z.size() == 15000);
  CHECK(z[5] == 42);
  CHECK(scan_col(db, c, 1)[5] == -1);
  // 4. The rewrite.
  sm3::column_conv_fn conv = [](std::span<const sm2::datum_t> v,
                                std::span<const bool> n, std::string &out,
                                bool &out_null) {
    out_null = n[0];
    int64_t a = out_null ? 0 : sm2::datum_to_int32(v[0]);
    out.assign(reinterpret_cast<const char *>(&a), 8);
    return 0;
  };
  db.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(db.ss, c, "a",
                                         {sm2::type_int8.code, -1}, &conv) == 0,
                  sm1::errmsg());
  db.commit();
  CHECK(row_groups(db, c) == 1);
  z = scan_col(db, c, 2);
  REQUIRE(z.size() == 15000);
  CHECK(z.begin()->first == 0);
  CHECK(z.rbegin()->first == 14999);
  CHECK(z[12345] == 42);
  // 5. TRUNCATE.
  db.begin();
  REQUIRE_MESSAGE(sm3::truncate_table(db.ss, c) == 0, sm1::errmsg());
  db.commit();
  CHECK(row_groups(db, c) == 0);
  CHECK(scan_col(db, c, 2).empty());
}

namespace {

/* The rows of c's row groups, as their RG keys count them. */
uint64_t rg_rows(col_db_t &db, uint64_t c) {
  uint64_t n = 0;
  db.begin();
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(db.cat, c);
  std::vector<std::pair<uint64_t, std::string>> sets;
  REQUIRE(sm1::fileset_list(db.s, def->storeid, sets) == 0);
  for (const auto &[rg, value] : sets) {
    sm3::rg_meta_t meta;
    REQUIRE(sm3::rg_meta_decode(value, meta) == 0);
    n += meta.rows;
  }
  db.commit();
  return n;
}

/* Delete the first n live rows of file set rg of c, by their names. */
void delete_in(col_db_t &db, uint64_t c, uint64_t rg, size_t n) {
  std::vector<std::pair<sm3::place_t, uint64_t>> ids;
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (ids.size() < n && sm3::tbl_scannxt(scan, id, tuple) == 0) {
    if (sm3::tbl_scan_place(scan).fsid == rg) {
      ids.emplace_back(sm3::tbl_scan_place(scan), id);
    }
  }
  sm3::tbl_scanend(scan);
  REQUIRE(ids.size() == n);
  for (auto [place, x] : ids) {
    REQUIRE_MESSAGE(sm3::tbl_delete_at(t, place, x) == 0, sm1::errmsg());
  }
  sm3::tbl_close(t);
  db.commit();
}

} // namespace

// Verify the sealer eliminates a row group under half live: its
// rows go into the delta store, and the seal takes them again with
// others (../COLSTORE.md §11.3).
// 1. Load 10,000 rows; attach a sealer; delete 6,000: the row group
//    is queued, and, 4,000 live, eliminated.
// 2. No row group: the 4,000 rows are in the delta store, once each.
// 3. Insert 6,000 rows: the seal makes one row group of 10,000.
TEST_CASE("the sealer eliminates a row group under half live") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows; the sealer; the deletes.
  load_rows(db, c, 10000);
  std::vector<uint64_t> first = rg_ids(db, c);
  REQUIRE(first.size() == 1);
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  delete_in(db, c, first[0], 6000);
  sm3::sealer_wait_idle(db.cat);
  // 2. In the delta store.
  CHECK(row_groups(db, c) == 0);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 4000);
  CHECK(rows.count(5999) == 0);
  CHECK(rows.count(6000) == 1);
  CHECK(rows.count(9999) == 1);
  CHECK(where(db, c, 6000).first.fsid == 0);
  // 3. Sealed again.
  insert_rows(db, c, 10000, 16000);
  sm3::sealer_wait_idle(db.cat);
  CHECK(row_groups(db, c) == 1);
  CHECK(rg_rows(db, c) == 10000);
  CHECK(where(db, c, 6000).first.fsid != 0);
  CHECK(scan_rows(db, c).size() == 10000);
  sm3::sealer_detach(db.cat);
}

// Verify an update of a row group's rows counts their new versions
// in the delta store toward the seal (../COLSTORE.md §10.4), with
// row groups of 100 rows.
// 1. Load 100 rows: one row group. Attach a sealer.
// 2. Update every row by its name, and commit: 100 new versions in
//    the delta store, and the row group all dead.
// 3. The sealer seals the delta into a new row group of 100 rows,
//    and compacts the old one away.
TEST_CASE("an update of a row group's rows counts toward the seal") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The row group, and the sealer.
  load_rows(db, c, 100);
  std::vector<uint64_t> old = rg_ids(db, c);
  REQUIRE(old.size() == 1);
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  // 2. Update every row.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  std::vector<std::tuple<sm3::place_t, uint64_t, int32_t>> rows;
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    rows.emplace_back(sm3::tbl_scan_place(scan), id, sm2::datum_to_int32(d[0]));
  }
  sm3::tbl_scanend(scan);
  REQUIRE(rows.size() == 100);
  for (auto [rg, x, a] : rows) {
    row_t r(a, "u");
    REQUIRE_MESSAGE(sm3::tbl_update_at(t, rg, x, r.d, r.null) == 0,
                    sm1::errmsg());
  }
  sm3::tbl_close(t);
  db.commit();
  // 3. Sealed, and the old row group gone.
  sm3::sealer_wait_idle(db.cat);
  std::vector<uint64_t> now = rg_ids(db, c);
  REQUIRE(now.size() == 1);
  CHECK(now[0] != old[0]);
  CHECK(rg_rows(db, c) == 100);
  auto after = scan_rows(db, c);
  CHECK(after.size() == 100);
  CHECK(after[99] == "u");
  sm3::sealer_detach(db.cat);
}

namespace {

/* Change one byte, at offset at, of the file of c's home named name. */
void flip_byte(col_db_t &db, const std::string &name, size_t at) {
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(db.path())) {
    if (e.is_regular_file() && e.path().filename() == name &&
        e.path().string().find("/colstore/") != std::string::npos) {
      std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
      REQUIRE(f.good());
      f.seekg(static_cast<std::streamoff>(at));
      char b = 0;
      f.read(&b, 1);
      b = static_cast<char>(b ^ 0x5a);
      f.seekp(static_cast<std::streamoff>(at));
      f.write(&b, 1);
      return;
    }
  }
  FAIL("no file " << name);
}

/* A scan of c's columns cols: 0 if it reads every row, else the error. */
int scan_error(col_db_t &db, uint64_t c, std::vector<bool> cols) {
  db.begin(sm1::xctlevel_t::repeatable_read);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_columns(t, cols, {}, 0, UINT64_MAX, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
  }
  int err = rc < 0 ? static_cast<int>(sm1::errnum()) : 0;
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(db.ss) == 0);
  return err;
}

} // namespace

// Verify that a changed byte in a CG file fails the scan that reads
// it with SM_ERR_CORRUPT, and not a scan of other columns
// (../COLSTORE.md §5.3), with row groups of 100 rows.
// 1. Load 100 rows whose b is 2,000 bytes: one row group.
// 2. Change a byte in b's CG file: a scan of a alone succeeds; a scan
//    of b fails.
// 3. Change a byte in a's CG file: a scan of a fails too.
TEST_CASE("a changed byte in a CG file fails the scan") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE(sm3::tbl_load_open(t, l) == 0);
  for (int i = 0; i < 100; i++) {
    row_t r(i, std::string(2000, static_cast<char>('a' + i % 26)));
    uint64_t id = 0;
    REQUIRE_MESSAGE(sm3::tbl_load_add(l, r.d, r.null, id) == 0, sm1::errmsg());
  }
  REQUIRE(sm3::tbl_load_close(l) == 0);
  sm3::tbl_close(t);
  db.commit();
  std::vector<uint64_t> rgs = rg_ids(db, c);
  REQUIRE(rgs.size() == 1);
  const std::string rg = std::to_string(rgs[0]);
  REQUIRE(scan_error(db, c, {}) == 0);
  // 2. b's CG.
  flip_byte(db, rg + ".1", 100);
  CHECK(scan_error(db, c, {true, false}) == 0);
  CHECK(scan_error(db, c, {}) == sm1::SM_ERR_CORRUPT);
  // 3. a's CG.
  flip_byte(db, rg + ".0", 10);
  CHECK(scan_error(db, c, {true, false}) == sm1::SM_ERR_CORRUPT);
}

namespace {

/* The regular files under the colstore directories of db's home. */
size_t colstore_files(col_db_t &db) {
  size_t n = 0;
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(db.path())) {
    n += e.is_regular_file() &&
         e.path().string().find("/colstore/") != std::string::npos;
  }
  return n;
}

} // namespace

// Verify that a reader's FOR SHARE lock keeps a row from elimination,
// and not from RG compaction, which keeps the lock; and that a
// prepared writer keeps a row from RG compaction and from the seal,
// until they end, with row groups of 100 rows.
// 1. Load 100 rows: one row group. S2 locks a = 5 FOR SHARE:
//    elimination gives up; RG compaction writes version 1, and S2's
//    lock still holds a = 5 from a FOR UPDATE. S2 commits.
// 2. S2 deletes a = 6 and prepares: RG compaction gives up. COMMIT
//    PREPARED: RG compaction writes version 2, 99 rows.
// 3. Insert 100 rows into the delta store. S2 updates a = 1050 and
//    prepares: the seal finds 99 rows it can take, and seals none.
//    ROLLBACK PREPARED: the seal takes all 100.
TEST_CASE("a FOR SHARE lock and a prepared writer hold off RG compaction "
          "and the seal") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  load_rows(db, c, 100);
  uint64_t rg = where(db, c, 5).first.fsid;
  REQUIRE(rg != 0);
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  sm3::table_t *t2 = nullptr;
  // 1. FOR SHARE.
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IS, t2) == 0);
  uint64_t id5 = where(db, c, 5).second;
  REQUIRE(sm3::tbl_lock(t2, id5, sm1::SM_LOCKMODE_SH) == 0);
  CHECK_FALSE(eliminate(ss3, c, rg));
  CHECK(where(db, c, 5).first.fsid == rg);
  CHECK(compact(ss3, c, rg));
  CHECK(where(db, c, 5).first.fsid == rg + 1);
  {
    REQUIRE(sm3::begin(ss3, sm1::xctlevel_t::read_committed) == 0);
    sm3::table_t *t3 = nullptr;
    REQUIRE(sm3::tbl_open(ss3, c, sm1::SM_LOCKMODE_IX, t3) == 0);
    CHECK(sm3::tbl_trylock(t3, id5, sm1::SM_LOCKMODE_EX) == 1);
    sm3::tbl_close(t3);
    REQUIRE(sm3::commit(ss3) == 0);
  }
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  rg = rg + 1;
  // 2. A prepared delete.
  auto [rg6, id6] = where(db, c, 6);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE(sm3::tbl_delete_at(t2, rg6, id6) == 0);
  sm3::tbl_close(t2);
  REQUIRE_MESSAGE(sm3::prepare(ss2, "g1") == 0, sm1::errmsg());
  CHECK_FALSE(compact(ss3, c, {rg}));
  REQUIRE_MESSAGE(sm1::commit_prepared(db.s, "g1") == 0, sm1::errmsg());
  CHECK(compact(ss3, c, {rg}));
  CHECK(where(db, c, 5).first.fsid == rg + 1);
  CHECK(scan_rows(db, c).size() == 99);
  // 3. A prepared update of a delta row.
  insert_rows(db, c, 1000, 1100);
  auto [rg0, id1050] = where(db, c, 1050);
  REQUIRE(rg0.fsid == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  row_t r(1050, "p");
  REQUIRE(sm3::tbl_update_at(t2, {}, id1050, r.d, r.null) == 0);
  sm3::tbl_close(t2);
  REQUIRE_MESSAGE(sm3::prepare(ss2, "g2") == 0, sm1::errmsg());
  CHECK_FALSE(seal(ss3, c));
  CHECK(where(db, c, 1000).first.fsid == 0);
  REQUIRE_MESSAGE(sm1::abort_prepared(db.s, "g2") == 0, sm1::errmsg());
  CHECK(seal(ss3, c));
  CHECK(where(db, c, 1000).first.fsid != 0);
  CHECK(where(db, c, 1050).first.fsid != 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify that a bulk load rolled back to a savepoint leaves no row,
// no row group and no file, and the transaction goes on, with row
// groups of 100 rows.
// 1. Begin, set a savepoint, and load 250 rows: two row groups and
//    50 rows in the delta store, with their files.
// 2. Roll back to the savepoint, insert one row, and commit.
// 3. One row, no row group, and no file.
TEST_CASE("a bulk load rolled back to a savepoint leaves nothing") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The load.
  db.begin();
  int depth = sm3::savepoint(db.ss);
  REQUIRE(depth >= 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE(sm3::tbl_load_open(t, l) == 0);
  for (int i = 0; i < 250; i++) {
    row_t r(i, "l");
    uint64_t id = 0;
    REQUIRE(sm3::tbl_load_add(l, r.d, r.null, id) == 0);
  }
  REQUIRE(sm3::tbl_load_close(l) == 0);
  sm3::tbl_close(t);
  CHECK(colstore_files(db) > 0);
  // 2. Back to the savepoint.
  REQUIRE_MESSAGE(sm3::rollback(db.ss, depth) == 0, sm1::errmsg());
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r(7, "kept");
  uint64_t id = 0;
  REQUIRE(sm3::tbl_insert(t, r.d, r.null, id) == 0);
  sm3::tbl_close(t);
  db.commit();
  // 3. What is left.
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 1);
  CHECK(rows[7] == "kept");
  CHECK(row_groups(db, c) == 0);
  CHECK(colstore_files(db) == 0);
}

namespace {

/* Every row of table oid, a to b, in ss's open transaction. */
std::map<int32_t, std::string> rows_in(sm3::session_t *ss, uint64_t oid) {
  std::map<int32_t, std::string> out;
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    std::string_view bytes;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], bytes) == 0);
    int32_t a = sm2::datum_to_int32(d[0]);
    CHECK(out.count(a) == 0);
    out[a] = std::string(sm2::varlena_payload(bytes));
  }
  REQUIRE_MESSAGE(rc == 1, sm1::errmsg());
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  return out;
}

} // namespace

// Verify a column store table agrees with a row store table under one
// random workload, with seals and RG compactions between, for a scan
// at each step's end and for an old REPEATABLE READ snapshot, with row
// groups of 100 rows (../COLSTORE.md §15.3).
// 1. Make c, a column store table, and r, a row store table; attach a
//    sealer, which seals and compacts c as its commits queue them.
// 2. Run 1,500 steps, each in a transaction at READ COMMITTED or
//    REPEATABLE READ, at random: an insert, an update by _id, an
//    update by (rg_id, _id), or a delete, of the same row in both.
//    Now and then, seal or compact c by hand.
// 3. Every 50 steps, c, r and the model agree.
// 4. Now and then, S2 takes an RR snapshot and keeps the model as it
//    was; 40 steps on, S2 reads c and r: both are as kept.
TEST_CASE("a column store table agrees with a row store table") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  uint64_t r = make(db, "r", false);
  db.commit();
  // 1. The sealer.
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  std::mt19937 rng(20261120);
  std::map<int32_t, std::string> model, kept;
  std::map<int32_t, std::pair<uint64_t, uint64_t>> ids; /* a: c's, r's */
  int next_a = 0, snap_at = -1;
  auto pick = [&]() {
    auto it = model.begin();
    std::advance(it, rng() % model.size());
    return it->first;
  };
  // 2. The steps.
  for (int step = 0; step < 1500; step++) {
    unsigned op = model.size() < 50 ? 0 : rng() % 10;
    db.begin(rng() % 2 ? sm1::xctlevel_t::repeatable_read
                       : sm1::xctlevel_t::read_committed);
    sm3::table_t *tc = nullptr, *tr = nullptr;
    REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, tc) == 0);
    REQUIRE(sm3::tbl_open(db.ss, r, sm1::SM_LOCKMODE_IX, tr) == 0);
    if (op < 4) {
      int32_t a = next_a++;
      row_t row(a, "i" + std::to_string(step));
      uint64_t idc = 0, idr = 0;
      REQUIRE(sm3::tbl_insert(tc, row.d, row.null, idc) == 0);
      REQUIRE(sm3::tbl_insert(tr, row.d, row.null, idr) == 0);
      ids[a] = {idc, idr};
      model[a] = "i" + std::to_string(step);
    } else if (op < 8) {
      int32_t a = pick();
      row_t row(a, "u" + std::to_string(step));
      if (op < 6) {
        REQUIRE_MESSAGE(sm3::tbl_update(tc, ids[a].first, row.d, row.null) == 0,
                        sm1::errmsg());
      } else {
        sm3::tbl_close(tc);
        sm3::tbl_close(tr);
        db.commit();
        auto [rg, id] = where(db, c, a);
        REQUIRE(id == ids[a].first);
        db.begin(sm1::xctlevel_t::read_committed);
        REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, tc) == 0);
        REQUIRE(sm3::tbl_open(db.ss, r, sm1::SM_LOCKMODE_IX, tr) == 0);
        REQUIRE_MESSAGE(sm3::tbl_update_at(tc, rg, id, row.d, row.null) == 0,
                        std::string(sm1::errmsg()));
      }
      REQUIRE(sm3::tbl_update(tr, ids[a].second, row.d, row.null) == 0);
      model[a] = "u" + std::to_string(step);
    } else {
      int32_t a = pick();
      REQUIRE_MESSAGE(sm3::tbl_delete(tc, ids[a].first) == 0, sm1::errmsg());
      REQUIRE(sm3::tbl_delete(tr, ids[a].second) == 0);
      model.erase(a);
      ids.erase(a);
    }
    sm3::tbl_close(tc);
    sm3::tbl_close(tr);
    db.commit();
    // By hand: a seal, or an RG compaction of one of c's row groups.
    // The sealer's jobs end first, so neither takes the other's rows.
    if (rng() % 25 == 0) {
      sm3::sealer_wait_idle(db.cat);
      seal(ss3, c);
    } else if (rng() % 25 == 0) {
      sm3::sealer_wait_idle(db.cat);
      std::vector<uint64_t> rgs = rg_ids(db, c);
      if (!rgs.empty()) {
        compact(ss3, c, {rgs[rng() % rgs.size()]});
      }
    }
    // 3. Agreement.
    if (step % 50 == 49) {
      sm3::sealer_wait_idle(db.cat);
      db.begin(sm1::xctlevel_t::repeatable_read);
      auto in_c = rows_in(db.ss, c);
      auto in_r = rows_in(db.ss, r);
      db.commit();
      REQUIRE(in_r == model);
      REQUIRE(in_c == model);
    }
    // 4. An old snapshot.
    if (snap_at < 0 && rng() % 60 == 0) {
      REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
      sm3::table_t *t = nullptr;
      REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IS, t) == 0);
      sm3::tbl_close(t);
      CHECK(rows_in(ss2, r) == model);
      kept = model;
      snap_at = step;
    } else if (snap_at >= 0 && step - snap_at >= 40) {
      CHECK(rows_in(ss2, c) == kept);
      CHECK(rows_in(ss2, r) == kept);
      REQUIRE(sm3::commit(ss2) == 0);
      snap_at = -1;
    }
  }
  if (snap_at >= 0) {
    REQUIRE(sm3::commit(ss2) == 0);
  }
  CHECK(rg_ids(db, c).size() > 1);
  sm3::sealer_detach(db.cat);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify ANALYZE's null fraction of a column store table counts the
// nulls of its row groups and of its delta store (../COLSTORE.md
// §12.1), with row groups of 100 rows.
// 1. Load 1,000 rows, b null in every fourth: ten row groups.
// 2. Insert 50 rows into the delta store, b null in every second.
// 3. ANALYZE at target 10, whose sample of 3,000 takes every row: a
//    has no null; b's null fraction is (250 + 25) / 1,050.
TEST_CASE("ANALYZE's null fraction counts row groups and the delta") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The row groups.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE(sm3::tbl_load_open(t, l) == 0);
  for (int i = 0; i < 1000; i++) {
    row_t r(i, "v");
    r.null[1] = i % 4 == 0;
    uint64_t id = 0;
    REQUIRE(sm3::tbl_load_add(l, r.d, r.null, id) == 0);
  }
  REQUIRE(sm3::tbl_load_close(l) == 0);
  // 2. The delta store.
  for (int i = 1000; i < 1050; i++) {
    row_t r(i, "d");
    r.null[1] = i % 2 == 0;
    uint64_t id = 0;
    REQUIRE(sm3::tbl_insert(t, r.d, r.null, id) == 0);
  }
  sm3::tbl_close(t);
  db.commit();
  REQUIRE(row_groups(db, c) == 10);
  // 3. ANALYZE.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  std::mt19937_64 rng(7);
  REQUIRE_MESSAGE(sm3::analyze_table(t, 10, rng) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  db.begin();
  std::shared_ptr<const sm3::stats_t> st;
  REQUIRE(sm3::cat_get_stats(db.ss, c, st) == 0);
  REQUIRE(st != nullptr);
  CHECK(st->live_rows == 1050.0);
  CHECK(st->rows_sampled == 1050);
  REQUIRE(st->stats.size() == 2);
  CHECK(st->stats[0].null_frac == 0.0f);
  CHECK(std::abs(st->stats[1].null_frac - 275.0f / 1050.0f) < 0.0001f);
  db.commit();
}

namespace {

/* The TOAST chunks of table oid that ss's open transaction sees. */
size_t toast_chunks(sm3::session_t *ss, sm1::session_t *s, uint64_t oid) {
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(ss, oid);
  std::string lo = "\xff\xff", hi = lo + std::string(32, '\xff');
  sm1::scan_t *c = nullptr;
  REQUIRE(sm1::scan(s, def->storeid, lo.data(), lo.size(), 1, hi.data(),
                    hi.size(), 1, c) == 0);
  size_t n = 0;
  std::string_view k, d;
  int rc;
  while ((rc = sm1::scannxt(c, k, d)) == 0) {
    n++;
  }
  sm1::scanend(c);
  REQUIRE(rc == 1);
  return n;
}

/* Every row of table oid, a to b whole, in ss's open transaction. */
std::map<int32_t, std::string> whole_rows(sm3::session_t *ss, uint64_t oid) {
  std::map<int32_t, std::string> out;
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    std::string_view bytes;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], bytes) == 0);
    // A value out of line is fetched, with no varlena header.
    std::string full(sm2::varlena_payload(bytes));
    if (sm2::varlena_is_toast(bytes)) {
      REQUIRE_MESSAGE(sm3::tbl_fetch(t, bytes, full) == 0, sm1::errmsg());
    }
    out[sm2::datum_to_int32(d[0])] = full;
  }
  REQUIRE_MESSAGE(rc == 1, sm1::errmsg());
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  return out;
}

} // namespace

// Verify the seal erases the TOAST chunks of the rows it takes, and a
// snapshot older than its commit still reads them (../COLSTORE.md
// §10.1), with row groups of 100 rows and TOAST on.
// 1. Insert 100 rows whose b is 2,000 bytes: in the delta store, b
//    out of line, in TOAST chunks, as the tuple is past 2,000 bytes.
// 2. S2 takes an RR snapshot, and reads every row.
// 3. Seal: a row group of 100 rows; no TOAST chunk is left.
// 4. A scan gives every b whole, from the row group's CG; S2 still
//    reads every b whole, from the chunks.
TEST_CASE("the seal erases its rows' TOAST chunks") {
  col_db_t db(100, true);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  auto b_of = [](int i) { return std::string(2000, 'a' + i % 26); };
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  for (int i = 0; i < 100; i++) {
    row_t r(i, b_of(i));
    uint64_t id = 0;
    REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d, r.null, id) == 0, sm1::errmsg());
  }
  sm3::tbl_close(t);
  db.commit();
  db.begin();
  CHECK(toast_chunks(db.ss, db.s, c) >= 100);
  db.commit();
  // 2. An older snapshot.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::tbl_close(t);
  REQUIRE(toast_chunks(ss2, s2, c) >= 100);
  // 3. The seal.
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  REQUIRE(seal(ss3, c));
  CHECK(row_groups(db, c) == 1);
  db.begin();
  CHECK(toast_chunks(db.ss, db.s, c) == 0);
  db.commit();
  // 4. Every b whole, now and at the older snapshot.
  db.begin(sm1::xctlevel_t::repeatable_read);
  auto now = whole_rows(db.ss, c);
  db.commit();
  REQUIRE(now.size() == 100);
  auto old = whole_rows(ss2, c);
  REQUIRE(old.size() == 100);
  for (int i = 0; i < 100; i++) {
    CHECK(now[i] == b_of(i));
    CHECK(old[i] == b_of(i));
  }
  REQUIRE(sm3::commit(ss2) == 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify a scan's bounds on a text column skip only the row groups
// that cannot match: a CG's keys are of its values' payloads, past
// their varlena headers, as the bounds' are (../COLSTORE.md §6), with
// row groups of 100 rows.
// 1. Load 300 rows: b is "aa" in the first row group, "mm" in the
//    second, "zz" in the third.
// 2. b < 'b', 'm' <= b < 'n', and b = 'zz' each give one row group's
//    100 rows; b > 'zzz' gives none.
TEST_CASE("a scan's bounds on a text column skip only what cannot match") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE(sm3::tbl_load_open(t, l) == 0);
  for (int i = 0; i < 300; i++) {
    row_t r(i, i < 100 ? "aa" : i < 200 ? "mm" : "zz");
    uint64_t id = 0;
    REQUIRE(sm3::tbl_load_add(l, r.d, r.null, id) == 0);
  }
  REQUIRE(sm3::tbl_load_close(l) == 0);
  sm3::tbl_close(t);
  db.commit();
  REQUIRE(row_groups(db, c) == 3);
  db.begin();
  const sm2::collation_t *coll =
      sm3::cat_get_table(db.ss, c)->columns[1].collation;
  db.commit();
  auto key = [&](std::string_view v) {
    std::string k;
    REQUIRE(sm2::type_text.key_encode(v, -1, coll, k) == 0);
    return k;
  };
  // 2. The bounds.
  sm3::col_bound_t lt{1, {}};
  lt.bound.not_null = lt.bound.has_hi = true;
  lt.bound.hi_incl = false;
  lt.bound.hi = key("b");
  CHECK(scan_rows(db, c, {}, {lt}).size() == 100);
  sm3::col_bound_t range{1, {}};
  range.bound.not_null = range.bound.has_lo = range.bound.has_hi = true;
  range.bound.lo = key("m");
  range.bound.hi_incl = false;
  range.bound.hi = key("n");
  CHECK(scan_rows(db, c, {}, {range}).size() == 100);
  sm3::col_bound_t eq{1, {}};
  eq.bound.not_null = eq.bound.has_in = true;
  eq.bound.in = {key("zz")};
  CHECK(scan_rows(db, c, {}, {eq}).size() == 100);
  sm3::col_bound_t past{1, {}};
  past.bound.not_null = past.bound.has_lo = true;
  past.bound.lo_incl = false;
  past.bound.lo = key("zzz");
  CHECK(scan_rows(db, c, {}, {past}).empty());
}

// Verify a second write of a row of a row group in one statement fails
// with SM_ERR_SELFMODIFIED, as a row store table's does, and a later
// statement's sees the first, with row groups of 100 rows.
// 1. Load 100 rows: one row group.
// 2. In one statement, delete a = 5 by its name twice: the second
//    fails with SM_ERR_SELFMODIFIED; so does an update of it after.
// 3. In one statement, update a = 6 by its name twice: the same.
// 4. In the next statement, a delete of a = 5 by its name is
//    SM_ERR_NOTFOUND, and of a = 6 SM_ERR_UPDATED: its new version is
//    in the delta store.
TEST_CASE("a second write of a row group's row in a statement is "
          "self-modified") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 100);
  auto [rg5, id5] = where(db, c, 5);
  auto [rg6, id6] = where(db, c, 6);
  REQUIRE(rg5.fsid != 0);
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r5(5, "u"), r6(6, "u");
  // 2. Two deletes.
  REQUIRE(sm3::stmt_begin(db.ss) == 0);
  REQUIRE(sm3::tbl_delete_at(t, rg5, id5) == 0);
  CHECK(sm3::tbl_delete_at(t, rg5, id5) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
  CHECK(sm3::tbl_update_at(t, rg5, id5, r5.d, r5.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
  // 3. Two updates.
  REQUIRE(sm3::tbl_update_at(t, rg6, id6, r6.d, r6.null) == 0);
  CHECK(sm3::tbl_update_at(t, rg6, id6, r6.d, r6.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
  // 4. The next statement.
  REQUIRE(sm3::stmt_begin(db.ss) == 0);
  CHECK(sm3::tbl_delete_at(t, rg5, id5) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::tbl_delete_at(t, rg6, id6) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  sm3::tbl_close(t);
  db.commit();
  CHECK(scan_rows(db, c).size() == 99);
}

// Verify a prepared transaction's delta rows and delete markers count
// toward the seal and RG compaction at its COMMIT PREPARED, and not
// after ROLLBACK PREPARED, with row groups of 100 rows.
// 1. Attach a sealer. Insert 60 rows and prepare; COMMIT PREPARED;
//    insert 40 more: the 100 are a row group's worth, and are sealed.
// 2. Delete 20 of its rows and prepare; COMMIT PREPARED: a fifth dead,
//    the row group is compacted into one of 80 rows.
// 3. Insert 100 rows and prepare; ROLLBACK PREPARED: nothing is
//    sealed, and 80 rows are left.
TEST_CASE("a prepared transaction's rows count at COMMIT PREPARED") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  // S2 prepares; a session with a prepared transaction begins no
  // other until it is resolved.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  auto insert_prepared = [&](int from, int to, const char *gid) {
    REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t) == 0);
    for (int i = from; i < to; i++) {
      row_t r(i, "p");
      uint64_t id = 0;
      REQUIRE(sm3::tbl_insert(t, r.d, r.null, id) == 0);
    }
    sm3::tbl_close(t);
    REQUIRE_MESSAGE(sm3::prepare(ss2, gid) == 0, sm1::errmsg());
  };
  // 1. Inserts.
  insert_prepared(0, 60, "g1");
  sm3::sealer_wait_idle(db.cat);
  CHECK(row_groups(db, c) == 0);
  REQUIRE_MESSAGE(sm3::commit_prepared(db.ss, "g1") == 0, sm1::errmsg());
  insert_rows(db, c, 60, 100);
  sm3::sealer_wait_idle(db.cat);
  std::vector<uint64_t> rgs = rg_ids(db, c);
  REQUIRE(rgs.size() == 1);
  // 2. Deletes.
  std::vector<std::pair<sm3::place_t, uint64_t>> names;
  for (int a = 0; a < 20; a++) {
    names.push_back(where(db, c, a));
  }
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t) == 0);
  for (auto [rg, id] : names) {
    REQUIRE(sm3::tbl_delete_at(t, rg, id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE_MESSAGE(sm3::prepare(ss2, "g2") == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm3::commit_prepared(db.ss, "g2") == 0, sm1::errmsg());
  sm3::sealer_wait_idle(db.cat);
  std::vector<uint64_t> after = rg_ids(db, c);
  REQUIRE(after.size() == 1);
  CHECK(after[0] != rgs[0]);
  CHECK(rg_rows(db, c) == 80);
  // 3. An abort.
  insert_prepared(1000, 1100, "g3");
  REQUIRE_MESSAGE(sm3::abort_prepared(db.ss, "g3") == 0, sm1::errmsg());
  sm3::sealer_wait_idle(db.cat);
  CHECK(rg_ids(db, c) == after);
  CHECK(scan_rows(db, c).size() == 80);
  sm3::sealer_detach(db.cat);
  sm3::session_close(ss2);
}

// Verify a column store table takes a varlena value of 2,048 bytes at
// most (../COLSTORE.md §5.4), on every write, and a row store table
// takes a longer one, with row groups of 100 rows.
// 1. An insert, a bulk load's row and an update in the delta store of
//    2,048 bytes are taken; of 2,049, refused with SM_ERR_VALUETOOLONG.
// 2. An update of a row of a row group by its name, of 2,049 bytes,
//    is refused; a null is taken.
// 3. ADD COLUMN with a missing value of 2,049 bytes is refused.
// 4. A row store table takes 2,049 bytes.
TEST_CASE("a column store table takes a value of 2,048 bytes at most") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  uint64_t r = make(db, "r", false);
  db.commit();
  row_t fits(1, std::string(2048, 'f')), over(2, std::string(2049, 'o'));
  // 1. The delta store's writes, and a load's.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  uint64_t id = 0, id2 = 0;
  REQUIRE(sm3::tbl_insert(t, fits.d, fits.null, id) == 0);
  CHECK(sm3::tbl_insert(t, over.d, over.null, id2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VALUETOOLONG);
  CHECK(sm3::tbl_update(t, id, over.d, over.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VALUETOOLONG);
  REQUIRE(sm3::tbl_update(t, id, fits.d, fits.null) == 0);
  sm3::tbl_close(t);
  db.commit();
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::loader_t *l = nullptr;
  REQUIRE(sm3::tbl_load_open(t, l) == 0);
  for (int i = 0; i < 100; i++) {
    row_t v(10 + i, std::string(2048, 'l'));
    REQUIRE(sm3::tbl_load_add(l, v.d, v.null, id2) == 0);
  }
  CHECK(sm3::tbl_load_add(l, over.d, over.null, id2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VALUETOOLONG);
  sm3::tbl_load_free(l);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(db.ss) == 0);
  // 2. A row of a row group.
  load_rows(db, c, 100);
  auto [rg, rid] = where(db, c, 50);
  REQUIRE(rg.fsid != 0);
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  CHECK(sm3::tbl_update_at(t, rg, rid, over.d, over.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VALUETOOLONG);
  row_t none(50, "");
  none.null[1] = true;
  REQUIRE(sm3::tbl_update_at(t, rg, rid, none.d, none.null) == 0);
  sm3::tbl_close(t);
  db.commit();
  // 3. ADD COLUMN.
  db.begin();
  std::string missing = sm2::make_varlena(std::string(2049, 'm'));
  sm3::column_spec_t z{"z", sm2::type_text.code, -1, false};
  CHECK(sm3::add_column(db.ss, c, z, missing) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VALUETOOLONG);
  REQUIRE(sm3::abort(db.ss) == 0);
  // 4. A row store table.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, r, sm1::SM_LOCKMODE_IX, t) == 0);
  CHECK(sm3::tbl_insert(t, over.d, over.null, id2) == 0);
  sm3::tbl_close(t);
  db.commit();
}

namespace {

/* Fold file set fsid of c in a transaction of its own; done. */
bool fold(sm3::session_t *ss, uint64_t c, uint64_t fsid) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool done = false;
  REQUIRE_MESSAGE(sm3::tbl_fold(t, fsid, done) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return done;
}

/* The rows of file set fsid of c deleted: by its validmap, by markers. */
std::pair<uint32_t, uint32_t> dead_of(col_db_t &db, uint64_t c, uint64_t fsid,
                                      uint32_t rows) {
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  std::vector<bool> dead;
  uint32_t ndead = 0, markers = 0;
  REQUIRE(sm3::col_dead(t, fsid, rows, dead, ndead, markers) == 0);
  sm3::tbl_close(t);
  db.commit();
  return {ndead - markers, markers};
}

/* The rows of c a transaction of ss counts, by a scan. */
size_t count_in(sm3::session_t *ss, uint64_t c) {
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  size_t n = 0;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    n++;
  }
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  return n;
}

} // namespace

// Verify a fold moves a row group's delete markers into its validmap,
// and that a snapshot older than the fold counts the same rows.
// 1. Load 10,000 rows, and delete a = 0 .. 149 by their names.
// 2. R (REPEATABLE READ) counts 9,850 rows.
// 3. Fold the row group: its 150 markers become 150 validmap bits.
// 4. R counts 9,850 still, and so does a new transaction.
// 5. A second fold finds no marker, and changes nothing.
TEST_CASE("a fold moves markers into the validmap; snapshots agree") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows, some deleted.
  load_rows(db, c, 10000);
  uint64_t fsid = where(db, c, 500).first.fsid;
  REQUIRE(fsid != 0);
  delete_rows(db, c, 0, 150);
  CHECK(dead_of(db, c, fsid, 10000) == std::pair<uint32_t, uint32_t>(0, 150));
  // 2. R, before the fold.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(count_in(ss2, c) == 9850);
  // 3. The fold.
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  CHECK(fold(ss3, c, fsid));
  CHECK(dead_of(db, c, fsid, 10000) == std::pair<uint32_t, uint32_t>(150, 0));
  // 4. Every snapshot counts the same rows.
  CHECK(count_in(ss2, c) == 9850);
  REQUIRE(sm3::commit(ss2) == 0);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 9850);
  CHECK(rows.count(149) == 0);
  CHECK(rows.count(150) == 1);
  // 5. Nothing more to fold.
  CHECK(fold(ss3, c, fsid));
  CHECK(dead_of(db, c, fsid, 10000) == std::pair<uint32_t, uint32_t>(150, 0));
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify a fold gives up while a writer of the row group holds IX on
// it, and that writers of different rows of one row group do not wait
// for each other.
// 1. Load 10,000 rows. S1 and S2, at REPEATABLE READ, each delete a
//    row of the one row group, and neither waits.
// 2. A fold gives up while they are in flight, and writes nothing.
// 3. Both commit; the fold now moves both markers.
TEST_CASE("a fold gives up on a writer; writers of one RG do not wait") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  load_rows(db, c, 10000);
  auto [p1, id1] = where(db, c, 10);
  auto [p2, id2] = where(db, c, 20);
  REQUIRE(p1.fsid == p2.fsid);
  // 1. Two writers of the row group.
  sm3::session_t *ss[2] = {nullptr, nullptr};
  sm3::table_t *t[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; i++) {
    REQUIRE(sm3::session_open(db.cat, db.open(), ss[i]) == 0);
    REQUIRE(sm3::begin(ss[i], sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(sm3::tbl_open(ss[i], c, sm1::SM_LOCKMODE_IX, t[i]) == 0);
  }
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t[0], p1, id1) == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t[1], p2, id2) == 0, sm1::errmsg());
  // 2. The fold, while they hold IX.
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  CHECK_FALSE(fold(ss3, c, p1.fsid));
  // 3. Both commit.
  for (int i = 0; i < 2; i++) {
    sm3::tbl_close(t[i]);
    REQUIRE_MESSAGE(sm3::commit(ss[i]) == 0, sm1::errmsg());
  }
  CHECK(dead_of(db, c, p1.fsid, 10000) == std::pair<uint32_t, uint32_t>(0, 2));
  CHECK(fold(ss3, c, p1.fsid));
  CHECK(dead_of(db, c, p1.fsid, 10000) == std::pair<uint32_t, uint32_t>(2, 0));
  CHECK(scan_rows(db, c).size() == 9998);
  for (int i = 0; i < 2; i++) {
    sm3::session_close(ss[i]);
  }
  sm3::session_close(ss3);
}

// Verify a write of a row whose delete a fold has folded fails: at
// REPEATABLE READ with a snapshot from before the delete, with
// SM_ERR_CONFLICT; at READ COMMITTED, with SM_ERR_NOTFOUND.
// 1. Load 10,000 rows; R (REPEATABLE READ) reads a = 7 and keeps its
//    name.
// 2. Another transaction deletes a = 7; a fold folds it.
// 3. R's delete of a = 7 fails with SM_ERR_CONFLICT.
// 4. A READ COMMITTED delete at the same name fails with
//    SM_ERR_NOTFOUND.
TEST_CASE("a write of a folded row fails as a write of a deleted one") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  load_rows(db, c, 10000);
  auto [p7, id7] = where(db, c, 7);
  // 1. R, before the delete.
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(count_in(ss2, c) == 10000);
  // 2. The delete, folded.
  delete_rows(db, c, 7, 8);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  CHECK(fold(ss3, c, p7.fsid));
  // 3. R's delete.
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  CHECK(sm3::tbl_delete_at(t2, p7, id7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm3::tbl_close(t2);
  REQUIRE(sm3::abort(ss2) == 0);
  // 4. At READ COMMITTED.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  CHECK(sm3::tbl_delete_at(t, p7, id7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(db.ss) == 0);
  CHECK(scan_rows(db, c).size() == 9999);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify the sealer folds a row group whose committed delete markers
// reach 1% of a row group's rows.
// 1. Load 10,000 rows, with the sealer attached.
// 2. Delete 120 rows by name, and commit: past 100 markers.
// 3. When the sealer is idle, the row group has 120 validmap bits and
//    no marker; a scan gives 9,880 rows.
TEST_CASE("the sealer folds a row group past 1% markers") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows, and the sealer.
  load_rows(db, c, 10000);
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  uint64_t fsid = where(db, c, 500).first.fsid;
  // 2. The deletes.
  delete_rows(db, c, 0, 120);
  // 3. Folded.
  sm3::sealer_wait_idle(db.cat);
  CHECK(dead_of(db, c, fsid, 10000) == std::pair<uint32_t, uint32_t>(120, 0));
  CHECK(scan_rows(db, c).size() == 9880);
  sm3::sealer_detach(db.cat);
}

namespace {

/* Where row a is as ss's transaction sees it: (place, _id). */
std::pair<sm3::place_t, uint64_t> where_in(sm3::session_t *ss, uint64_t c,
                                           int32_t a) {
  std::pair<sm3::place_t, uint64_t> at{{UINT64_MAX, 0}, 0};
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    if (!null[0] && sm2::datum_to_int32(d[0]) == a) {
      at = {sm3::tbl_scan_place(scan), id};
    }
  }
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  return at;
}

/* Whether ss's transaction sees a row with a = a in c. */
bool sees(sm3::session_t *ss, uint64_t c, int32_t a) {
  return where_in(ss, c, a).first.fsid != UINT64_MAX;
}

} // namespace

// Verify RG compaction writes the next version of a row group in
// place, with its live rows alone, and that a snapshot older than it
// still reads the version it saw.
// 1. Load 10,000 rows; R (REPEATABLE READ) counts them.
// 2. Delete a = 0 .. 1,999, and compact: version 1 of the same row
//    group, of 8,000 rows; a = 2,000 is at rno 0.
// 3. R counts 10,000 still; a new transaction counts 8,000.
TEST_CASE("RG compaction writes the next version of a row group") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows; R.
  load_rows(db, c, 10000);
  uint64_t fsid = where(db, c, 0).first.fsid;
  REQUIRE(sm3::fsid_version(fsid) == 0);
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(count_in(ss2, c) == 10000);
  // 2. The deletes, and the compaction.
  delete_rows(db, c, 0, 2000);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  CHECK(compact(ss3, c, {fsid}));
  auto [p, id] = where(db, c, 2000);
  CHECK(p.fsid == fsid + 1);
  CHECK(sm3::fsid_rg(p.fsid) == sm3::fsid_rg(fsid));
  CHECK(p.rno == 0);
  CHECK(row_groups(db, c) == 1);
  CHECK(rg_rows(db, c) == 8000);
  // 3. Each snapshot, its version.
  CHECK(count_in(ss2, c) == 10000);
  REQUIRE(sm3::commit(ss2) == 0);
  CHECK(scan_rows(db, c).size() == 8000);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify a REPEATABLE READ writer whose snapshot is older than two
// compactions deletes the right row, at its newest place, and does
// not see it afterwards.
// 1. Load 10,000 rows; R (REPEATABLE READ) finds a = 5,000 at rno
//    5,000 of version 0.
// 2. Delete a = 0 .. 1,999 and compact; delete a = 2,000 .. 3,599
//    and compact again: version 2 holds a = 5,000 at rno 1,400.
// 3. R deletes a = 5,000 by its old place: R no longer sees it, and
//    sees a = 4,999 and a = 5,001.
// 4. R commits: a new transaction counts 6,399 rows, without
//    a = 5,000.
TEST_CASE("a writer older than two compactions deletes the right row") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows; R's name for a = 5,000.
  load_rows(db, c, 10000);
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p0, id0] = where_in(ss2, c, 5000);
  REQUIRE(sm3::fsid_version(p0.fsid) == 0);
  REQUIRE(p0.rno == 5000);
  // 2. Two compactions.
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  delete_rows(db, c, 0, 2000);
  CHECK(compact(ss3, c, {p0.fsid}));
  delete_rows(db, c, 2000, 3600);
  CHECK(compact(ss3, c, {p0.fsid + 1}));
  auto [p2, id2] = where(db, c, 5000);
  CHECK(p2.fsid == p0.fsid + 2);
  CHECK(p2.rno == 1400);
  CHECK(id2 == id0);
  // 3. R's delete, by its old place.
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t2, p0, id0) == 0, sm1::errmsg());
  sm3::tbl_close(t2);
  CHECK_FALSE(sees(ss2, c, 5000));
  CHECK(sees(ss2, c, 4999));
  CHECK(sees(ss2, c, 5001));
  CHECK(count_in(ss2, c) == 9999);
  // 4. Committed.
  REQUIRE_MESSAGE(sm3::commit(ss2) == 0, sm1::errmsg());
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 6399);
  CHECK(rows.count(5000) == 0);
  CHECK(rows.count(5001) == 1);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

// Verify two writers of one row, of snapshots on each side of a
// compaction, meet at the row's newest place: the second fails.
// 1. Load 10,000 rows; R (REPEATABLE READ) finds a = 3,000.
// 2. Delete a = 0 .. 1,999, and compact.
// 3. W, newer, deletes a = 3,000 at its newest place, and commits.
// 4. R's delete of a = 3,000 by its old place fails with
//    SM_ERR_CONFLICT.
TEST_CASE("writers of a row on each side of a compaction conflict") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows; R.
  load_rows(db, c, 10000);
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p0, id0] = where_in(ss2, c, 3000);
  // 2. The compaction.
  delete_rows(db, c, 0, 2000);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  CHECK(compact(ss3, c, {p0.fsid}));
  // 3. W.
  delete_rows(db, c, 3000, 3001);
  // 4. R.
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  CHECK(sm3::tbl_delete_at(t2, p0, id0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm3::tbl_close(t2);
  sm3::abort(ss2);
  CHECK(scan_rows(db, c).size() == 7999);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

namespace {

/* Clean up old version fsid of c in a transaction of its own; done. */
bool cleanup(sm3::session_t *ss, uint64_t c, uint64_t fsid) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool done = false;
  REQUIRE_MESSAGE(sm3::tbl_cleanup(t, fsid, done) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return done;
}

/* The delete markers of file set fsid of c a new transaction sees. */
size_t markers_of(col_db_t &db, uint64_t c, uint64_t fsid) {
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IS, t) == 0);
  std::vector<uint32_t> rnos;
  REQUIRE(sm3::col_markers(t, fsid, rnos) == 0);
  sm3::tbl_close(t);
  db.commit();
  return rnos.size();
}

} // namespace

// Verify the markers of an old version, a shadow marker among them,
// go once no snapshot sees the version, and not before.
// 1. Load 10,000 rows; R (REPEATABLE READ) finds a = 5,000.
// 2. Delete a = 0 .. 1,999, and compact: version 0 keeps 2,000
//    markers. R deletes a = 5,000 by its old place: a shadow marker
//    in version 0 too.
// 3. The cleanup of version 0 waits while R's snapshot sees it.
// 4. R commits: the cleanup erases version 0's 2,001 markers, and the
//    rows are as they were.
TEST_CASE("an old version's markers go once no snapshot sees it") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows; R.
  load_rows(db, c, 10000);
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p0, id0] = where_in(ss2, c, 5000);
  // 2. The compaction; R's delete.
  delete_rows(db, c, 0, 2000);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  CHECK(compact(ss3, c, {p0.fsid}));
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t2, p0, id0) == 0, sm1::errmsg());
  sm3::tbl_close(t2);
  // 3. Not yet.
  CHECK_FALSE(cleanup(ss3, c, p0.fsid));
  CHECK(markers_of(db, c, p0.fsid) == 2000);
  // 4. Now.
  REQUIRE_MESSAGE(sm3::commit(ss2) == 0, sm1::errmsg());
  CHECK(markers_of(db, c, p0.fsid) == 2001);
  CHECK(cleanup(ss3, c, p0.fsid));
  CHECK(markers_of(db, c, p0.fsid) == 0);
  CHECK(markers_of(db, c, p0.fsid + 1) == 1);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 7999);
  CHECK(rows.count(5000) == 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
}

namespace {

/* Compact c's row group three times, from its version 0: version 3. */
uint64_t to_version3(col_db_t &db, sm3::session_t *ss, uint64_t c) {
  uint64_t fsid = where(db, c, 9999).first.fsid;
  REQUIRE(sm3::fsid_version(fsid) == 0);
  // 10,000, then 8,000, 6,400 and 5,120 rows: a fifth each time.
  delete_rows(db, c, 0, 2000);
  REQUIRE(compact(ss, c, fsid));
  delete_rows(db, c, 2000, 3600);
  REQUIRE(compact(ss, c, fsid + 1));
  delete_rows(db, c, 3600, 4880);
  REQUIRE(compact(ss, c, fsid + 2));
  return fsid + 3;
}

/* Update row a of c in ss's transaction, by place p and _id id. */
int update_at(sm3::session_t *ss, uint64_t c, sm3::place_t p, uint64_t id,
              int32_t a, const char *b) {
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r(a, b);
  int rc = sm3::tbl_update_at(t, p, id, r.d, r.null);
  sm3::tbl_close(t);
  return rc;
}

} // namespace

// Verify elimination puts a version 3's live rows into the delta
// store, and that writers of snapshots older than it write the row
// there, or fail where it changed since.
// 1. Load 10,000 rows, and compact three times: version 3, of 5,120
//    rows, a = 4,880 .. 9,999. R and R2 (REPEATABLE READ) see it.
// 2. W updates a = 6,000 at its place, and commits. Compact version
//    3: it is eliminated, its 5,120 live rows in the delta store.
// 3. R reads version 3 still. R deletes a = 5,000 by its old place:
//    R no longer sees it. R2's update of a = 6,000 fails with
//    SM_ERR_CONFLICT.
// 4. R commits: a new transaction finds no row group, 5,119 rows, in
//    the delta store, and W's a = 6,000.
// 5. No snapshot sees version 3: the cleanup erases its markers, R's
//    shadow marker among them.
TEST_CASE("an RR writer older than an elimination writes the moved-in row") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. Version 3; R and R2.
  load_rows(db, c, 10000);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  uint64_t v3 = to_version3(db, ss3, c);
  sm3::session_t *ss2 = nullptr, *ss4 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::session_open(db.cat, db.open(), ss4) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::begin(ss4, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p5, id5] = where_in(ss2, c, 5000);
  auto [p6, id6] = where_in(ss4, c, 6000);
  REQUIRE(p5.fsid == v3);
  REQUIRE(p6.fsid == v3);
  // 2. W's update; the elimination.
  db.begin();
  REQUIRE_MESSAGE(update_at(db.ss, c, p6, id6, 6000, "w") == 0, sm1::errmsg());
  db.commit();
  CHECK(compact(ss3, c, v3));
  CHECK(row_groups(db, c) == 0);
  CHECK(where(db, c, 5000).first.fsid == 0);
  // 3. R and R2.
  CHECK(count_in(ss2, c) == 5120);
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t2, p5, id5) == 0, sm1::errmsg());
  sm3::tbl_close(t2);
  CHECK_FALSE(sees(ss2, c, 5000));
  CHECK(sees(ss2, c, 5001));
  CHECK(count_in(ss2, c) == 5119);
  CHECK(update_at(ss4, c, p6, id6, 6000, "r2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm3::abort(ss4);
  // 4. Committed.
  REQUIRE_MESSAGE(sm3::commit(ss2) == 0, sm1::errmsg());
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 5119);
  CHECK(rows.count(5000) == 0);
  CHECK(rows[6000] == "w");
  CHECK(row_groups(db, c) == 0);
  // 5. The cleanup.
  CHECK(markers_of(db, c, v3) > 0);
  CHECK(cleanup(ss3, c, v3));
  CHECK(markers_of(db, c, v3) == 0);
  sm3::session_close(ss2);
  sm3::session_close(ss3);
  sm3::session_close(ss4);
}

// Verify a writer by _id alone, as digres names a row it has not
// just scanned, hides the row from its own reads of the version its
// snapshot sees, after a compaction and after an elimination.
// 1. Load 10,000 rows; R (REPEATABLE READ) sees version 0. Delete
//    a = 0 .. 1,999, and compact into version 1.
// 2. R deletes a = 5,000 by its _id: R no longer sees it, and counts
//    9,999 rows.
// 3. R2 (REPEATABLE READ) sees version 3, after two more
//    compactions; version 3 is eliminated. R2 updates a = 6,000 by
//    its _id: R2 counts 5,119 rows, and a = 6,000 once.
TEST_CASE("a writer by _id older than a compaction hides the row it wrote") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. R; version 1.
  load_rows(db, c, 10000);
  sm3::session_t *ss2 = nullptr, *ss3 = nullptr, *ss4 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  REQUIRE(sm3::session_open(db.cat, db.open(), ss4) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p5, id5] = where_in(ss2, c, 5000);
  REQUIRE(sm3::fsid_version(p5.fsid) == 0);
  delete_rows(db, c, 0, 2000);
  REQUIRE(compact(ss3, c, p5.fsid));
  // 2. R's delete by _id.
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete(t, id5) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  CHECK_FALSE(sees(ss2, c, 5000));
  CHECK(count_in(ss2, c) == 9999);
  REQUIRE_MESSAGE(sm3::commit(ss2) == 0, sm1::errmsg());
  // 3. R2; version 3, eliminated; R2's update by _id.
  delete_rows(db, c, 2000, 3600);
  REQUIRE(compact(ss3, c, p5.fsid + 1));
  delete_rows(db, c, 3600, 4880);
  REQUIRE(compact(ss3, c, p5.fsid + 2));
  REQUIRE(sm3::begin(ss4, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p6, id6] = where_in(ss4, c, 6000);
  REQUIRE(p6.fsid == p5.fsid + 3);
  CHECK(count_in(ss4, c) == 5119);
  REQUIRE(compact(ss3, c, p5.fsid + 3));
  REQUIRE(row_groups(db, c) == 0);
  REQUIRE(sm3::tbl_open(ss4, c, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r(6000, "r2");
  REQUIRE_MESSAGE(sm3::tbl_update(t, id6, r.d, r.null) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  CHECK(count_in(ss4, c) == 5119);
  REQUIRE_MESSAGE(sm3::commit(ss4) == 0, sm1::errmsg());
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 5119);
  CHECK(rows[6000] == "r2");
  sm3::session_close(ss2);
  sm3::session_close(ss3);
  sm3::session_close(ss4);
}

// Verify a delete and an update by _id alone of rows of a row group,
// in one statement, write once each, as digres's DELETE ... WHERE _id
// = n does: no shadow marker at the place just written, whose marker
// the statement does not see yet.
// 1. Load 100 rows: one row group.
// 2. In a statement, delete a = 5 and update a = 6 by their _ids.
// 3. Commit: 99 rows, a = 6 updated.
TEST_CASE("a write by _id in a statement marks its row once") {
  col_db_t db(100);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  load_rows(db, c, 100);
  uint64_t id5 = where(db, c, 5).second;
  uint64_t id6 = where(db, c, 6).second;
  // 2. The statement.
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::stmt_begin(db.ss) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete(t, id5) == 0, sm1::errmsg());
  row_t r6(6, "u");
  REQUIRE_MESSAGE(sm3::tbl_update(t, id6, r6.d, r6.null) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  db.commit();
  // 3. Once each.
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 99);
  CHECK(rows.count(5) == 0);
  CHECK(rows[6] == "u");
}

// Verify the sealer's first pass finds an old version whose cleanup
// a restart lost, and cleans it up.
// 1. Load 10,000 rows; delete a = 0 .. 1,999, and compact with no
//    sealer attached: version 0 keeps its 2,000 markers, and no
//    cleanup is queued, as after a restart.
// 2. Attach a sealer: its first pass finds version 0, and the cleanup
//    erases its markers.
TEST_CASE("the sealer's first pass cleans up an old version a restart left") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. Version 0 left.
  load_rows(db, c, 10000);
  uint64_t v0 = where(db, c, 5000).first.fsid;
  delete_rows(db, c, 0, 2000);
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  REQUIRE(compact(ss3, c, v0));
  REQUIRE(markers_of(db, c, v0) == 2000);
  // 2. The sealer; the cleanup comes within a pass or two.
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  size_t left = markers_of(db, c, v0);
  for (int i = 0; i < 100 && left != 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    left = markers_of(db, c, v0);
  }
  CHECK(left == 0);
  CHECK(scan_rows(db, c).size() == 8000);
  sm3::sealer_detach(db.cat);
  sm3::session_close(ss3);
}

// Verify the cleanup of an old version waits for a prepared writer of
// its row group across a restart, whose snapshot no longer holds the
// watermark back, and does not block on its shadow marker.
// 1. Load 10,000 rows; R (REPEATABLE READ) finds a = 5,000. Delete
//    a = 0 .. 1,999, and compact: version 1.
// 2. R deletes a = 5,000 by its old place, a shadow marker in version
//    0, and prepares. Restart.
// 3. The cleanup of version 0 gives up while R is prepared.
// 4. COMMIT PREPARED: the cleanup erases version 0's markers; 7,999
//    rows.
TEST_CASE("the cleanup waits for a prepared writer across a restart") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. R; version 1.
  load_rows(db, c, 10000);
  sm3::session_t *ss2 = nullptr, *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), ss2) == 0);
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  auto [p0, id0] = where_in(ss2, c, 5000);
  delete_rows(db, c, 0, 2000);
  REQUIRE(compact(ss3, c, p0.fsid));
  // 2. R's delete, prepared; the restart.
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t, p0, id0) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE_MESSAGE(sm3::prepare(ss2, "gc") == 0, sm1::errmsg());
  sm3::session_close(ss2);
  sm3::session_close(ss3);
  db.reload();
  REQUIRE(sm3::session_open(db.cat, db.open(), ss3) == 0);
  // 3. Not while R is prepared.
  CHECK_FALSE(cleanup(ss3, c, p0.fsid));
  CHECK(markers_of(db, c, p0.fsid) == 2000);
  // 4. Now.
  REQUIRE_MESSAGE(sm3::commit_prepared(db.ss, "gc") == 0, sm1::errmsg());
  CHECK(cleanup(ss3, c, p0.fsid));
  CHECK(markers_of(db, c, p0.fsid) == 0);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 7999);
  CHECK(rows.count(5000) == 0);
  sm3::session_close(ss3);
}
