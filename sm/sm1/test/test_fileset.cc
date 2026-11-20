/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_fileset.cc -- file sets (src/fileset.cc, ../COLSTORE.md §4.5,
 * §13): files written durably, named by a versioned key, unlinked
 * by an abort, deleted by GC, and swept at open.
 */

#include "doctest.h"

#include "db.h"
#include "session.h"
#include "util.h"

#include <filesystem>

using namespace sm1_internal;

namespace fs = std::filesystem;

namespace {

/* A database "db" with one store, for file sets. */
class fs_fixture_t {
public:
  /* Create "db" and its store. */
  fs_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_s);
    REQUIRE(sm1::begin(m_s, sm1::xctlevel_t::repeatable_read) == 0);
    m_store = mkstore(m_s);
    REQUIRE(sm1::commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm1::session_t *s() const { return m_s; }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* The store. */
  uint64_t store() const { return m_store; }

  /* The path of file name of file set fsid. */
  fs::path path(uint64_t fsid, const std::string &name) const {
    return fs::path(m_s->db->rdb->GetName()) / "colstore" /
           std::to_string(m_store) / std::to_string(fsid / 1024) /
           (std::to_string(fsid) + "." + name);
  }

  /* Force a full compaction of the store, through the filter. */
  void compact() {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(
        m_s->db->rdb
            ->CompactRange(opt, db_store(m_s->db, m_store).cf, nullptr, nullptr)
            .ok());
  }

  /* Close the session, reattach as a new process, and reopen "db". */
  void reopen() {
    REQUIRE(sm1::closedb(m_s) == 0);
    m_home.reattach();
    m_s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_s);
  }

private:
  home_t m_home;
  sm1::session_t *m_s = nullptr;
  uint64_t m_store = 0;
};

/* Write file set fsid's files "1" and "2", and put its key. */
void make_set(sm1::session_t *s, uint64_t store, uint64_t fsid,
              const std::string &payload) {
  REQUIRE(sm1::fileset_write(s, store, fsid, "1", "one") == 0);
  REQUIRE(sm1::fileset_write(s, store, fsid, "2", "two") == 0);
  REQUIRE(sm1::fileset_put(s, store, fsid, payload) == 0);
}

/* The file sets the session's transaction sees. */
std::vector<std::pair<uint64_t, std::string>> listed(sm1::session_t *s,
                                                     uint64_t store) {
  std::vector<std::pair<uint64_t, std::string>> out;
  REQUIRE(sm1::fileset_list(s, store, out) == 0);
  return out;
}

} // namespace

// Verify a committed file set is listed with its payload and read back.
// 1. Write two files of file set 7, put its key, commit.
// 2. A new transaction lists it, and reads each file whole and in part.
// 3. A file name with a '/' is refused.
TEST_CASE("a committed file set is listed and its files read") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  // 1. Write and commit.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 7, "rg7");
  REQUIRE(sm1::commit(s) == 0);
  // 2. List and read.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  auto sets = listed(s, f.store());
  REQUIRE(sets.size() == 1);
  CHECK(sets[0].first == 7);
  CHECK(sets[0].second == "rg7");
  std::string out;
  REQUIRE(sm1::fileset_read(s, f.store(), 7, "1", 0, UINT64_MAX, out) == 0);
  CHECK(out == "one");
  REQUIRE(sm1::fileset_read(s, f.store(), 7, "2", 1, 2, out) == 0);
  CHECK(out == "wo");
  // 3. A bad name.
  CHECK(sm1::fileset_write(s, f.store(), 8, "a/b", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::commit(s) == 0);
}

// Verify an abort and a rollback to a savepoint unlink the files.
// 1. Write file set 1 and put its key; abort: its files are gone,
//    and no transaction lists it.
// 2. Write file set 2, set a savepoint, write file set 3; roll back:
//    3's files are gone, 2's stay; commit: only 2 is listed.
TEST_CASE("an abort and a rollback to a savepoint unlink a file set") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  // 1. Abort.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 1, "a");
  CHECK(fs::exists(f.path(1, "1")));
  REQUIRE(sm1::abort(s) == 0);
  CHECK_FALSE(fs::exists(f.path(1, "1")));
  CHECK_FALSE(fs::exists(f.path(1, "2")));
  // 2. Roll back past a savepoint.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 2, "b");
  int depth = sm1::savepoint(s);
  REQUIRE(depth == 1);
  make_set(s, f.store(), 3, "c");
  REQUIRE(sm1::rollback(s, depth) == 0);
  CHECK_FALSE(fs::exists(f.path(3, "1")));
  CHECK(fs::exists(f.path(2, "1")));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  auto sets = listed(s, f.store());
  REQUIRE(sets.size() == 1);
  CHECK(sets[0].first == 2);
  REQUIRE(sm1::commit(s) == 0);
}

// Verify the sweep at open deletes the files a crash left unnamed.
// 1. Commit file set 1.
// 2. Write file set 2 and put its key, and end the transaction
//    with its file list emptied, as a crash leaves the files.
// 3. After the open, 2's files are gone and 1's stay.
TEST_CASE("the sweep at open deletes a crashed transaction's files") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  // 1. A committed file set.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 1, "a");
  REQUIRE(sm1::commit(s) == 0);
  // 2. One in flight, through a session the reopen does not close.
  sm1::session_t *w = f.open();
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  make_set(w, f.store(), 2, "b");
  // A crash runs no abort: empty w's list of files, so the abort of
  // its close unlinks none, as a crash would not.
  w->fs_files.clear();
  REQUIRE(sm1::closedb(w) == 0);
  f.reopen();
  // 3. The sweep has run.
  CHECK(fs::exists(f.path(1, "1")));
  CHECK_FALSE(fs::exists(f.path(2, "1")));
  CHECK_FALSE(fs::exists(f.path(2, "2")));
}

// Verify a prepared file set's files survive a restart, and its fate
// then decides them.
// 1. Prepare file set 1 as "g1" and file set 2 as "g2"; reopen.
// 2. Both sets' files are there.
// 3. Commit g1 and abort g2: 1 is listed; compaction and GC delete
//    2's files.
TEST_CASE("a prepared file set survives a restart until its fate") {
  fs_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  // 1. Two prepared transactions.
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  make_set(a, f.store(), 1, "a");
  REQUIRE(sm1::prepare(a, "g1") == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  make_set(b, f.store(), 2, "b");
  REQUIRE(sm1::prepare(b, "g2") == 0);
  // Closing the sessions leaves both in doubt, for the restart.
  REQUIRE(sm1::closedb(a) == 0);
  REQUIRE(sm1::closedb(b) == 0);
  f.reopen();
  // 2. Kept by the sweep.
  CHECK(fs::exists(f.path(1, "1")));
  CHECK(fs::exists(f.path(2, "1")));
  // 3. Their fates.
  sm1::session_t *s = f.s();
  REQUIRE(sm1::commit_prepared(s, "g1") == 0);
  REQUIRE(sm1::abort_prepared(s, "g2") == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  auto sets = listed(s, f.store());
  REQUIRE(sets.size() == 1);
  CHECK(sets[0].first == 1);
  REQUIRE(sm1::commit(s) == 0);
  f.compact();
  gc_run_once(s->db);
  CHECK(fs::exists(f.path(1, "1")));
  CHECK_FALSE(fs::exists(f.path(2, "1")));
}

// Verify an erased file set stays readable to an older snapshot, and
// GC deletes its files once no snapshot sees it.
// 1. Commit file set 1; R begins and lists it.
// 2. Erase it and commit.
// 3. R still lists it and reads its files; a new transaction does not
//    list it.
// 4. R ends; compaction and GC purge the key, and the files go.
TEST_CASE("an erased file set's files go once no snapshot sees it") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  sm1::session_t *r = f.open();
  // 1. Committed, and seen by R.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 1, "a");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(listed(r, f.store()).size() == 1);
  // 2. Erased.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::fileset_erase(s, f.store(), 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  // 3. R sees it still; a new transaction does not.
  CHECK(listed(r, f.store()).size() == 1);
  f.compact();
  gc_run_once(s->db);
  std::string out;
  REQUIRE(sm1::fileset_read(r, f.store(), 1, "1", 0, UINT64_MAX, out) == 0);
  CHECK(out == "one");
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(listed(s, f.store()).empty());
  REQUIRE(sm1::commit(s) == 0);
  // 4. R ends: the files go.
  REQUIRE(sm1::commit(r) == 0);
  f.compact();
  gc_run_once(s->db);
  CHECK_FALSE(fs::exists(f.path(1, "1")));
  sm1::closedb(r);
}

// Verify an erase of a file set judges a newer commit and a missing set.
// 1. A REPEATABLE READ transaction R begins; S erases file set 1.
// 2. R's erase of 1 fails with SM_ERR_CONFLICT.
// 3. An erase of a set never put fails with SM_ERR_NOTFOUND.
TEST_CASE("an erase of a file set judges newer commits") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  sm1::session_t *r = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 1, "a");
  REQUIRE(sm1::commit(s) == 0);
  // 1. R, then S's erase.
  REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(listed(r, f.store()).size() == 1);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::fileset_erase(s, f.store(), 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  // 2. R's erase.
  CHECK(sm1::fileset_erase(r, f.store(), 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  // 3. A set never put.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::fileset_erase(s, f.store(), 99) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(s) == 0);
  sm1::closedb(r);
}

// Verify a file a visible key names that is missing is corruption.
// 1. Commit file set 1, then remove its file "1" behind sm1's back.
// 2. Reading it fails with SM_ERR_CORRUPT.
TEST_CASE("a missing file of a file set is corruption") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  make_set(s, f.store(), 1, "a");
  REQUIRE(sm1::commit(s) == 0);
  // 1. Gone behind sm1's back.
  fs::remove(f.path(1, "1"));
  // 2. Corruption.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::string out;
  CHECK(sm1::fileset_read(s, f.store(), 1, "1", 0, UINT64_MAX, out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  REQUIRE(sm1::commit(s) == 0);
}

// Verify a dropped store's file sets go with it.
// 1. Commit file set 1 in a second store.
// 2. Drop the store; once GC drops its column family, its directory
//    is gone.
TEST_CASE("a dropped store's file sets go with it") {
  fs_fixture_t f;
  sm1::session_t *s = f.s();
  // 1. A second store, with a file set.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t other = mkstore(s);
  REQUIRE(sm1::fileset_write(s, other, 1, "1", "x") == 0);
  REQUIRE(sm1::fileset_put(s, other, 1, "a") == 0);
  REQUIRE(sm1::commit(s) == 0);
  fs::path dir =
      fs::path(s->db->rdb->GetName()) / "colstore" / std::to_string(other);
  CHECK(fs::exists(dir));
  // 2. Drop it.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::rmstore(s, other) == 0);
  REQUIRE(sm1::commit(s) == 0);
  gc_run_once(s->db);
  CHECK_FALSE(fs::exists(dir));
}
