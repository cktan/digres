/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of ty_set_alloc(): the alloc and free functions of a thread.
 */
#include <stdlib.h>
#include <string.h>

#include <thread>

#include "doctest.h"
#include "mem.h"
#include "ty.h"
#include "ty_text.h"

namespace {

/* What a counting hook saw on its thread. */
struct count_t {
  int allocs = 0;
  int frees = 0;
};
thread_local count_t t_count;

/* An alloc that counts, and gives malloc's memory. */
void *counting_alloc(size_t size) {
  t_count.allocs++;
  return malloc(size);
}

/* A free that counts, and gives memory back to free. */
void counting_free(void *ptr) {
  t_count.frees++;
  free(ptr);
}

/* An alloc that cannot allocate. */
void *failing_alloc(size_t) { return nullptr; }

/* A free that gives memory back to free. */
void plain_free(void *ptr) { free(ptr); }

/* Back to the default for the thread, so a test leaves nothing. */
struct reset_t {
  ~reset_t() { ty_set_alloc(nullptr, nullptr); }
};

} // namespace

// Verify that a thread with no setting has malloc and free.
// 1. Allocate and free with no hook set.
// 2. Check that a counting hook was not called.
TEST_CASE("the default alloc is malloc and the default free is free") {
  t_count = count_t();
  // 1. Allocate and free with no hook set.
  void *p = ty_internal::mem_alloc(16);
  CHECK(nullptr != p);
  ty_internal::mem_free(p);
  // 2. Check that a counting hook was not called.
  CHECK(0 == t_count.allocs);
  CHECK(0 == t_count.frees);
}

// Verify that a set alloc and free get the calls, and that both NULL
// restores the default.
// 1. Set a counting alloc and free.
// 2. Allocate and free once: each hook is called once.
// 3. Set both NULL, then allocate and free once more.
// 4. Check that the counts did not move.
TEST_CASE("ty_set_alloc sets the functions, and both NULL restores them") {
  reset_t reset;
  t_count = count_t();
  // 1. Set a counting alloc and free.
  REQUIRE(0 == ty_set_alloc(counting_alloc, counting_free));
  // 2. Allocate and free once: each hook is called once.
  void *p = ty_internal::mem_alloc(16);
  CHECK(nullptr != p);
  ty_internal::mem_free(p);
  CHECK(1 == t_count.allocs);
  CHECK(1 == t_count.frees);
  // 3. Set both NULL, then allocate and free once more.
  REQUIRE(0 == ty_set_alloc(nullptr, nullptr));
  p = ty_internal::mem_alloc(16);
  ty_internal::mem_free(p);
  // 4. Check that the counts did not move.
  CHECK(1 == t_count.allocs);
  CHECK(1 == t_count.frees);
}

// Verify that only one of alloc and free fails and changes nothing.
// 1. Set a counting alloc and free.
// 2. Set alloc only, then free only: each fails with TY_ERR_INVALID.
// 3. Allocate once and check the counting hook still gets it.
TEST_CASE("ty_set_alloc fails when only one of the two is NULL") {
  reset_t reset;
  t_count = count_t();
  // 1. Set a counting alloc and free.
  REQUIRE(0 == ty_set_alloc(counting_alloc, counting_free));
  // 2. Set alloc only, then free only: each fails with TY_ERR_INVALID.
  CHECK(-1 == ty_set_alloc(counting_alloc, nullptr));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_set_alloc(nullptr, counting_free));
  CHECK(TY_ERR_INVALID == ty_errnum());
  // 3. Allocate once and check the counting hook still gets it.
  void *p = ty_internal::mem_alloc(16);
  ty_internal::mem_free(p);
  CHECK(1 == t_count.allocs);
  CHECK(1 == t_count.frees);
}

// Verify that an alloc that returns NULL gives NULL to the caller.
// 1. Set an alloc that fails, with a free.
// 2. Allocate: the result is NULL.
TEST_CASE("an alloc that fails gives NULL") {
  reset_t reset;
  // 1. Set an alloc that fails, with a free.
  REQUIRE(0 == ty_set_alloc(failing_alloc, plain_free));
  // 2. Allocate: the result is NULL.
  CHECK(nullptr == ty_internal::mem_alloc(16));
}

// Verify that the setting is per thread.
// 1. Set a counting alloc and free on this thread.
// 2. On a second thread, allocate with no setting, then set the
//    counting hook there, and allocate once more.
// 3. Check that each thread counted only its own calls.
TEST_CASE("the alloc and free functions belong to one thread") {
  reset_t reset;
  t_count = count_t();
  // 1. Set a counting alloc and free on this thread.
  REQUIRE(0 == ty_set_alloc(counting_alloc, counting_free));
  int other_unset = -1;
  int other_allocs = -1;
  int other_frees = -1;
  // 2. On a second thread, allocate with no setting, then set the
  //    counting hook there, and allocate once more.
  std::thread t([&] {
    t_count = count_t();
    void *p = ty_internal::mem_alloc(16);
    ty_internal::mem_free(p);
    other_unset = t_count.allocs;
    ty_set_alloc(counting_alloc, counting_free);
    p = ty_internal::mem_alloc(16);
    ty_internal::mem_free(p);
    other_allocs = t_count.allocs;
    other_frees = t_count.frees;
  });
  t.join();
  // 3. Check that each thread counted only its own calls.
  CHECK(0 == other_unset);
  CHECK(1 == other_allocs);
  CHECK(1 == other_frees);
  CHECK(0 == t_count.allocs);
  CHECK(0 == t_count.frees);
}

// Verify that ty_text_concat makes a || b in the thread's alloc memory, after
// the headroom, and reports a limit and an alloc that fails.
// 1. Concatenate two texts with a headroom of 4 bytes, with the counting hook.
// 2. Check the text is after the headroom, the length is right, and one alloc.
// 3. Concatenate with an empty side and no headroom.
// 4. Check a total past the limit is TY_ERR_RANGE, with no alloc.
// 5. Check an alloc that fails is TY_ERR_NOMEM.
TEST_CASE("ty_text_concat uses the thread's alloc and keeps the headroom") {
  reset_t reset;
  char *out = nullptr;
  size_t n = 0;

  // 1. Concatenate two texts with a headroom of 4 bytes, with the counting
  // hook.
  ty_set_alloc(counting_alloc, counting_free);
  t_count = count_t();
  REQUIRE(0 == ty_text_concat("ab", 2, "cde", 3, 4, &out, &n));
  // 2. Check the text is after the headroom, the length is right, and one
  // alloc.
  CHECK(5 == n);
  CHECK(0 == memcmp(out + 4, "abcde", 5));
  CHECK(1 == t_count.allocs);
  free(out);
  // 3. Concatenate with an empty side and no headroom.
  REQUIRE(0 == ty_text_concat("", 0, "xy", 2, 0, &out, &n));
  CHECK(2 == n);
  CHECK(0 == memcmp(out, "xy", 2));
  free(out);
  // 4. Check a total past the limit is TY_ERR_RANGE, with no alloc.
  t_count = count_t();
  CHECK(-1 == ty_text_concat("a", (size_t)0x3fffffff, "b", 1, 4, &out, &n));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(0 == t_count.allocs);
  // 5. Check an alloc that fails is TY_ERR_NOMEM.
  ty_set_alloc(failing_alloc, plain_free);
  CHECK(-1 == ty_text_concat("a", 1, "b", 1, 0, &out, &n));
  CHECK(TY_ERR_NOMEM == ty_errnum());
}
