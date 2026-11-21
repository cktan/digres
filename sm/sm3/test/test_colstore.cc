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

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
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

/* Where row a is: (rg_id, _id), read by a scan of c. */
std::pair<uint64_t, uint64_t> where(col_db_t &db, uint64_t c, int32_t a) {
  std::pair<uint64_t, uint64_t> at{UINT64_MAX, 0};
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
      at = {sm3::tbl_scan_rg(scan), id};
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
  CHECK(rg7 != 0);
  CHECK(rg8 == rg7);
  CHECK(rg12 == 0);
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
  CHECK(rg8b == 0);
  CHECK(id8b == id8);
  // 4. Delete in the delta store.
  db.begin();
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::tbl_delete_at(t, 0, id12) == 0);
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
  REQUIRE_MESSAGE(sm3::tbl_delete_at(t, 0, id10) == 0, sm1::errmsg());
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
  CHECK(where(db, c, 5).first != 0);
  CHECK(where(db, c, 11000).first == 0);
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
  CHECK(rg3 == 0);
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
  CHECK(where(db, c, 3).first == 0);
  CHECK(where(db, c, 4).first != 0);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 3. R, then a second seal, then R's update.
  insert_rows(db, c, 10001, 20001);
  auto [rgx, idx] = where(db, c, 15000);
  CHECK(rgx == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  std::string_view tuple;
  REQUIRE(sm3::tbl_read(t2, idx, tuple) == 0);
  CHECK(seal(ss3, c));
  CHECK(where(db, c, 15000).first != 0);
  row_t r(15000, "r");
  REQUIRE_MESSAGE(sm3::tbl_update_at(t2, 0, idx, r.d, r.null) == 0,
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

/* Compact row groups rgs of c in a transaction of its own; done. */
bool compact(sm3::session_t *ss, uint64_t c, std::vector<uint64_t> rgs) {
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  bool done = false;
  REQUIRE_MESSAGE(sm3::tbl_compact_rgs(t, rgs, done) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  return done;
}

/* Delete rows a = from .. to - 1 of c by their names. */
void delete_rows(col_db_t &db, uint64_t c, int from, int to) {
  std::vector<std::pair<uint64_t, uint64_t>> names;
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

// Verify RG compaction rewrites a row group's live rows into a new
// one, and gives up on a row with a writer in flight.
// 1. Load 10,000 rows: one row group; delete a = 0 .. 29.
// 2. S2 locks a = 50: compaction gives up, and nothing changes.
// 3. S2 commits: compaction makes a new row group of 9,970 rows, and
//    the old one is gone; a scan gives 9,970 rows, a = 50 among them.
TEST_CASE("RG compaction rewrites live rows, and gives up on a writer") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows, some deleted.
  load_rows(db, c, 10000);
  uint64_t rg = where(db, c, 100).first;
  REQUIRE(rg != 0);
  delete_rows(db, c, 0, 30);
  // 2. A writer in flight.
  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t2 = nullptr;
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  REQUIRE(sm3::tbl_lock(t2, where(db, c, 50).second, sm1::SM_LOCKMODE_EX) == 0);
  sm1::session_t *s3 = db.open();
  sm3::session_t *ss3 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s3, ss3) == 0);
  CHECK_FALSE(compact(ss3, c, {rg}));
  CHECK(where(db, c, 100).first == rg);
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  // 3. Now.
  CHECK(compact(ss3, c, {rg}));
  CHECK(row_groups(db, c) == 1);
  uint64_t rg2 = where(db, c, 100).first;
  CHECK(rg2 != 0);
  CHECK(rg2 != rg);
  auto rows = scan_rows(db, c);
  CHECK(rows.size() == 9970);
  CHECK(rows.count(50) == 1);
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
  CHECK(compact(ss3, c, {rg7}));
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
  uint64_t rg = where(db, c, 50).first;
  REQUIRE(rg != 0);
  REQUIRE(row_groups(db, c) == 1);
  // 2. Not yet.
  delete_rows(db, c, 0, 19);
  sm3::sealer_wait_idle(db.cat);
  CHECK(where(db, c, 50).first == rg);
  // 3. Now.
  delete_rows(db, c, 19, 20);
  sm3::sealer_wait_idle(db.cat);
  uint64_t rg2 = where(db, c, 50).first;
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
  REQUIRE(rg5 != 0);
  REQUIRE(rg6 == rg5);
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
  CHECK(where(db, c, 5).first == 0);
  // 5. A row RG compaction moved.
  auto [rg8, id8] = where(db, c, 8);
  REQUIRE(compact(db.ss, c, {rg8}));
  CHECK(where(db, c, 8).first != rg8);
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
    REQUIRE(rg != 0);
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
  REQUIRE(compact(db.ss, c, rg_ids(db, c)));
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

/* Delete the first n live rows of row group rg of c, by their names. */
void delete_in(col_db_t &db, uint64_t c, uint64_t rg, size_t n) {
  std::vector<uint64_t> ids;
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, c, sm1::SM_LOCKMODE_IX, t) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (ids.size() < n && sm3::tbl_scannxt(scan, id, tuple) == 0) {
    if (sm3::tbl_scan_rg(scan) == rg) {
      ids.push_back(id);
    }
  }
  sm3::tbl_scanend(scan);
  REQUIRE(ids.size() == n);
  for (uint64_t x : ids) {
    REQUIRE_MESSAGE(sm3::tbl_delete_at(t, rg, x) == 0, sm1::errmsg());
  }
  sm3::tbl_close(t);
  db.commit();
}

} // namespace

// Verify the sealer merges small row groups next to each other into
// one (../COLSTORE.md §11.1), and leaves a row group with nothing to
// gain as it is.
// 1. Load 10,000 rows, delete 6,000, and compact by hand: a row group
//    of 4,000, small.
// 2. Load 10,000 more, delete 7,000, and compact: a second small row
//    group, of 3,000, next to the first.
// 3. Attach a sealer, and queue the first: both merge into one row
//    group of 7,000 rows.
// 4. Queue it again: not small, and no row dead, it stays.
TEST_CASE("the sealer merges small row groups") {
  col_db_t db;
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The first small row group.
  load_rows(db, c, 10000);
  std::vector<uint64_t> first = rg_ids(db, c);
  REQUIRE(first.size() == 1);
  delete_in(db, c, first[0], 6000);
  REQUIRE(compact(db.ss, c, first));
  std::vector<uint64_t> small1 = rg_ids(db, c);
  REQUIRE(small1.size() == 1);
  // 2. The second.
  load_rows(db, c, 10000);
  std::vector<uint64_t> both = rg_ids(db, c);
  REQUIRE(both.size() == 2);
  uint64_t second = both[0] == small1[0] ? both[1] : both[0];
  delete_in(db, c, second, 7000);
  REQUIRE(compact(db.ss, c, {second}));
  REQUIRE(rg_ids(db, c).size() == 2);
  // 3. The merge.
  sm1::env_t *env = db.env();
  REQUIRE(sm3::sealer_attach(db.cat, [env]() -> sm1::session_t * {
            sm1::session_t *s = nullptr;
            return sm1::opendb(env, "db", s) == 0 ? s : nullptr;
          }) == 0);
  sm3::sealer_enqueue_rg(db.cat, c, small1[0]);
  sm3::sealer_wait_idle(db.cat);
  std::vector<uint64_t> merged = rg_ids(db, c);
  REQUIRE(merged.size() == 1);
  CHECK(merged[0] != small1[0]);
  CHECK(rg_rows(db, c) == 7000);
  // 4. Nothing to gain.
  sm3::sealer_enqueue_rg(db.cat, c, merged[0]);
  sm3::sealer_wait_idle(db.cat);
  CHECK(rg_ids(db, c) == merged);
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
  std::vector<std::tuple<uint64_t, uint64_t, int32_t>> rows;
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    sm2::datum_t d[2];
    bool null[2];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, null) == 0);
    rows.emplace_back(sm3::tbl_scan_rg(scan), id, sm2::datum_to_int32(d[0]));
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

// Verify that a changed byte in a CG file, or in a long value, fails
// the scan that reads it with SM_ERR_CORRUPT (../COLSTORE.md §5.3,
// §5.4), with row groups of 100 rows.
// 1. Load 100 rows whose b is a long value: one row group.
// 2. Change a byte in the long value of the first row: a scan of a
//    alone reads no long value, and succeeds; a scan of b fails.
// 3. Change a byte in a's CG file: a scan of a fails too.
TEST_CASE("a changed byte in a CG file or a long value fails the scan") {
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
  // 2. The long value.
  flip_byte(db, rg + ".1.long", 100);
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

// Verify that a reader's FOR SHARE lock and a prepared writer keep a
// row from RG compaction and from the seal, until they end, with row
// groups of 100 rows.
// 1. Load 100 rows: one row group. S2 locks a = 5 FOR SHARE: RG
//    compaction gives up. S2 commits: nothing yet changes.
// 2. S2 deletes a = 6 and prepares: RG compaction gives up. COMMIT
//    PREPARED: RG compaction rewrites the row group, 99 rows.
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
  uint64_t rg = where(db, c, 5).first;
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
  REQUIRE(sm3::tbl_lock(t2, where(db, c, 5).second, sm1::SM_LOCKMODE_SH) == 0);
  CHECK_FALSE(compact(ss3, c, {rg}));
  sm3::tbl_close(t2);
  REQUIRE(sm3::commit(ss2) == 0);
  CHECK(where(db, c, 5).first == rg);
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
  CHECK(where(db, c, 5).first != rg);
  CHECK(scan_rows(db, c).size() == 99);
  // 3. A prepared update of a delta row.
  insert_rows(db, c, 1000, 1100);
  auto [rg0, id1050] = where(db, c, 1050);
  REQUIRE(rg0 == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss2, c, sm1::SM_LOCKMODE_IX, t2) == 0);
  row_t r(1050, "p");
  REQUIRE(sm3::tbl_update_at(t2, 0, id1050, r.d, r.null) == 0);
  sm3::tbl_close(t2);
  REQUIRE_MESSAGE(sm3::prepare(ss2, "g2") == 0, sm1::errmsg());
  CHECK_FALSE(seal(ss3, c));
  CHECK(where(db, c, 1000).first == 0);
  REQUIRE_MESSAGE(sm1::abort_prepared(db.s, "g2") == 0, sm1::errmsg());
  CHECK(seal(ss3, c));
  CHECK(where(db, c, 1000).first != 0);
  CHECK(where(db, c, 1050).first != 0);
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
                        sm1::errmsg());
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
// 1. Insert 100 rows whose b is 3,000 bytes: in the delta store, b
//    out of line, in TOAST chunks.
// 2. S2 takes an RR snapshot, and reads every row.
// 3. Seal: a row group of 100 rows; no TOAST chunk is left.
// 4. A scan gives every b whole, from the row group's long values;
//    S2 still reads every b whole, from the chunks.
TEST_CASE("the seal erases its rows' TOAST chunks") {
  col_db_t db(100, true);
  db.begin();
  uint64_t c = make(db, "c", true);
  db.commit();
  // 1. The rows.
  auto b_of = [](int i) { return std::string(3000, 'a' + i % 26); };
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
  REQUIRE(rg5 != 0);
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
