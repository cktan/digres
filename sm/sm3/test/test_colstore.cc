/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_colstore.cc -- column store tables (../COLSTORE.md): their
 * kind in the catalog, what they refuse, and their data.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>
#include <vector>

namespace {

/* A catalog on a fresh database, loaded, with a session ss over s. */
class col_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  col_db_t() {
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

private:
  /* Load the catalog; open ss over s. */
  void load() {
    REQUIRE_MESSAGE(sm3::cat_open(env(), "db", cat) == 0, sm1::errmsg());
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
// 2. At SERIALIZABLE, c's open fails with SM_ERR_INVALID; r opens.
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
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
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
