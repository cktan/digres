/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of ty on a thread of sm: the alloc and free functions that sm's
 * threads give to ty_set_alloc(), an arena of the thread and a lambda
 * that reads it, as each thread's start does (gc.cc, for one).
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <string>
#include <thread>

#include "doctest.h"
#include "session.h"
#include "sm0_arena.h"
#include "sm1.h"
#include "ty.h"
#include "ty_text.h"
#include "util.h"

namespace {

/* A varlena of the bytes of s, with a short header (at most 126 bytes). */
std::string short_text(const std::string &s) {
  std::string v(1, (char)(((s.size() + 1) << 1) | 1));

  return v + s;
}

} // namespace

// Verify that a thread whose arena is given to ty has ty allocate from it,
// and that a reset of the arena frees the pieces at once.
// 1. On a new thread, make an arena and give ty a lambda that reads it.
// 2. Allocate twice with ty_alloc(): the pieces are aligned to 8 and apart.
// 3. Free the first: the second is still good, as free does nothing.
// 4. Concatenate two texts with ty_text_concat(): the result is right.
// 5. Reset the arena: the next piece is the first of the block again.
TEST_CASE("ty on a thread of sm allocates from the thread's arena") {
  bool ok = false;

  std::thread t([&] {
    // 1. On a new thread, make an arena and give ty a lambda that reads it.
    sm0::arena_t arena;
    static thread_local sm0::arena_t *t_arena;
    t_arena = &arena;
    ty_set_alloc(
        [](size_t n) -> void * { return t_arena->alloc((n + 7) & ~(size_t)7); },
        [](void *) {});

    // 2. Allocate twice with ty_alloc(): aligned to 8 and apart.
    char *a = static_cast<char *>(ty_alloc(5));
    char *b = static_cast<char *>(ty_alloc(5));
    if (nullptr == a || nullptr == b) {
      return;
    }
    memset(a, 'a', 5);
    memset(b, 'b', 5);
    bool apart = b >= a + 8 || a >= b + 8;

    // 3. Free the first: the second is still good, as free does nothing.
    ty_free(a);
    bool kept = 0 == memcmp(b, "bbbbb", 5);

    // 4. Concatenate two texts with ty_text_concat(): the result is right.
    std::string x = short_text("ab"), y = short_text("cde");
    ty_varlena_t *r = nullptr;
    bool concat =
        0 == ty_text_concat(reinterpret_cast<const ty_varlena_t *>(x.data()),
                            reinterpret_cast<const ty_varlena_t *>(y.data()),
                            &r) &&
        5 == ty_vl_len(r) && 0 == memcmp(ty_vl_data(r), "abcde", 5);
    ty_free(r);

    // 5. Reset the arena: the next piece is the first of the block again.
    arena.reset();
    bool reused = static_cast<char *>(ty_alloc(5)) == a;

    ok = 0 == ((uintptr_t)a & 7) && 0 == ((uintptr_t)b & 7) && apart && kept &&
         concat && reused;
  });
  t.join();
  CHECK(ok);
}

// Verify that each thread has an arena of its own, and that a thread that
// did not call ty_set_alloc() has ty's default, malloc and free.
// 1. Two threads each give ty their own arena and allocate.
// 2. Each thread gets a piece in its own arena and can write it.
// 3. This thread, which did not give one, allocates and frees with malloc.
TEST_CASE("each thread of sm has an arena of its own") {
  char *pa = nullptr, *pb = nullptr;

  // 1. Two threads each give ty their own arena and allocate.
  auto run = [](char *&out, int fill) {
    sm0::arena_t arena;
    static thread_local sm0::arena_t *t_arena;
    t_arena = &arena;
    ty_set_alloc(
        [](size_t n) -> void * { return t_arena->alloc((n + 7) & ~(size_t)7); },
        [](void *) {});
    out = static_cast<char *>(ty_alloc(16));
    memset(out, fill, 16);
  };
  std::thread t1([&] { run(pa, 1); });
  t1.join();
  std::thread t2([&] { run(pb, 2); });
  t2.join();

  // 2. Each thread gets a piece in its own arena and can write it.
  CHECK(nullptr != pa);
  CHECK(nullptr != pb);

  // 3. This thread, which did not give one, allocates and frees with malloc.
  void *p = ty_alloc(32);
  CHECK(nullptr != p);
  ty_free(p);
}

namespace {

/* The wall clock now, as a timestamptz (microseconds since 2000-01-01). */
int64_t now_timestamptz() {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return ((int64_t)ts.tv_sec - 946684800) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace

// Verify that sm1 saves the wall clock when a transaction takes its first
// snapshot, that sm1 and ty give the same time until the transaction ends,
// and that a later transaction has a later time.
// 1. With no transaction, sm1::xact_start fails with SM_ERR_NOTRANS.
// 2. Begin a transaction between two readings of the clock.
// 3. Read the start time from sm1 and from ty: both are in the two readings.
// 4. Wait, begin a statement, and read again: the time did not change.
// 5. Commit: sm1 fails with SM_ERR_NOTRANS, and ty has no start time.
// 6. Begin a second transaction: its time is later than the first.
// 7. Close the session: ty has no clock.
TEST_CASE("sm1 owns the start time of a transaction, and ty reads it") {
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db", nullptr);
  REQUIRE(s);
  int64_t t1 = 0, t2 = 0, ts = 0;

  // 1. With no transaction, sm1::xact_start fails with SM_ERR_NOTRANS.
  CHECK(-1 == sm1::xact_start(s, &ts));
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  // 2. Begin a transaction between two readings of the clock.
  int64_t before = now_timestamptz();
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  int64_t after = now_timestamptz();

  // 3. Read the start time from sm1 and from ty: both are in the readings.
  REQUIRE(0 == sm1::xact_start(s, &t1));
  CHECK(t1 >= before);
  CHECK(t1 <= after);
  REQUIRE(0 == ty_xact_start(&ts));
  CHECK(ts == t1);

  // 4. Wait, begin a statement, and read again: the time did not change.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(0 == sm1::stmt_begin(s));
  REQUIRE(0 == sm1::xact_start(s, &ts));
  CHECK(ts == t1);
  REQUIRE(0 == ty_xact_start(&ts));
  CHECK(ts == t1);

  // 5. Commit: sm1 fails with SM_ERR_NOTRANS, and ty has no start time.
  REQUIRE(0 == sm1::commit(s));
  CHECK(-1 == sm1::xact_start(s, &ts));
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  CHECK(-1 == ty_xact_start(&ts));

  // 6. Begin a second transaction: its time is later than the first.
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::repeatable_read));
  REQUIRE(0 == sm1::xact_start(s, &t2));
  CHECK(t2 > t1);
  REQUIRE(0 == ty_xact_start(&ts));
  CHECK(ts == t2);
  REQUIRE(0 == sm1::commit(s));

  // 7. Close the session: ty has no clock.
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  REQUIRE(0 == sm1::closedb(s));
  CHECK(-1 == ty_xact_start(&ts));
}

// Verify that a host may start the clock before it begins the transaction:
// the time is there at once, for sm1 and for ty, and the session keeps it
// through its own transactions, such as the work of a reaper.
// 1. Start the clock: sm1::xact_start and ty_xact_start give a time between
//    two readings of the wall clock, with no transaction.
// 2. Wait, then begin a transaction: the time is the one of the start.
// 3. Starting the clock inside the transaction fails with SM_ERR_INTRANS.
// 4. Commit: the time stays; a transaction of sm's own work on the session
//    (store_work_begin and store_work_end) does not change it either.
// 5. Start the clock again: the time is later.
// 6. Close the session: ty has no clock.
TEST_CASE("a host starts the clock of a transaction before sm1::begin") {
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db", nullptr);
  REQUIRE(s);
  int64_t ts = 0, tt = 0, started = 0;

  // A store for the work of 4, made before the clock starts.
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  uint64_t store = mkstore(s);
  REQUIRE(0 == sm1::commit(s));

  // 1. Start the clock: both give a time between two readings.
  int64_t before = now_timestamptz();
  REQUIRE(0 == sm1::xact_clock_start(s));
  int64_t after = now_timestamptz();
  REQUIRE(0 == sm1::xact_start(s, &started));
  CHECK(started >= before);
  CHECK(started <= after);
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);

  // 2. Wait, then begin a transaction: the time is the one of the start.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  REQUIRE(0 == sm1::xact_start(s, &ts));
  CHECK(ts == started);
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);

  // 3. Starting the clock inside the transaction fails with SM_ERR_INTRANS.
  CHECK(-1 == sm1::xact_clock_start(s));
  CHECK(sm1::errnum() == sm1::SM_ERR_INTRANS);

  // 4. Commit: the time stays; sm's own work on the session keeps it too.
  REQUIRE(0 == sm1::commit(s));
  REQUIRE(0 == sm1::xact_start(s, &ts));
  CHECK(ts == started);
  REQUIRE(0 == sm1::store_work_begin(s, store));
  sm1::store_work_end(s);
  REQUIRE(0 == sm1::xact_start(s, &ts));
  CHECK(ts == started);
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);

  // 5. Start the clock again: the time is later.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(0 == sm1::xact_clock_start(s));
  REQUIRE(0 == sm1::xact_start(s, &ts));
  CHECK(ts > started);

  // 6. Close the session: ty has no clock.
  REQUIRE(0 == sm1::closedb(s));
  CHECK(-1 == ty_xact_start(&tt));
}

// Verify that a second session on a thread does not take or clear the clock
// that ty reads for a session whose host started it, as the sessions sm opens
// on its caller's thread for its own work do not.
// 1. Open two sessions, and start the clock of the first.
// 2. Begin and commit a transaction on the second: ty still reads the first's.
// 3. Close the second: ty still reads the first's.
// 4. With no host clock, a second session that begins takes ty's clock only
//    when the one ty reads has no transaction.
// 5. Close the first: ty has no clock.
TEST_CASE("a second session on a thread does not disturb the host's clock") {
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *host = opendb(home.env(), "db", nullptr);
  sm1::session_t *other = opendb(home.env(), "db", nullptr);
  REQUIRE(host);
  REQUIRE(other);
  int64_t started = 0, tt = 0, t_other = 0;

  // 1. Open two sessions, and start the clock of the first.
  REQUIRE(0 == sm1::xact_clock_start(host));
  REQUIRE(0 == sm1::xact_start(host, &started));

  // 2. Begin and commit a transaction on the second: ty still reads the
  // first's.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(0 == sm1::begin(other, sm1::xctlevel_t::read_committed));
  REQUIRE(0 == sm1::xact_start(other, &t_other));
  CHECK(t_other > started);
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);
  REQUIRE(0 == sm1::commit(other));
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);

  // 3. Close the second: ty still reads the first's.
  REQUIRE(0 == sm1::closedb(other));
  REQUIRE(0 == ty_xact_start(&tt));
  CHECK(tt == started);

  // 4. A session of a host that never starts a clock is the only one that
  //    takes ty's clock, and only when ty has none to read.
  ty_set_xact_clock(nullptr);
  sm1::session_t *plain = opendb(home.env(), "db", nullptr);
  REQUIRE(plain);
  REQUIRE(0 == sm1::begin(plain, sm1::xctlevel_t::read_committed));
  REQUIRE(0 == ty_xact_start(&tt));
  REQUIRE(0 == sm1::xact_start(plain, &t_other));
  CHECK(tt == t_other);
  REQUIRE(0 == sm1::commit(plain));
  CHECK(-1 == ty_xact_start(&tt));
  REQUIRE(0 == sm1::closedb(plain));

  // 5. Close the first: ty has no clock.
  ty_set_xact_clock(&host->xact_start);
  REQUIRE(0 == sm1::closedb(host));
  CHECK(-1 == ty_xact_start(&tt));
}
