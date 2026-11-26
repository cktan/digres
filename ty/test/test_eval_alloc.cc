/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_eval_alloc.cc -- the evaluation engine when memory runs out: each
 * allocation of ty_prog_compile and ty_run_new fails in turn, and nothing
 * is left allocated. The test binary's operator new counts what is live,
 * and fails the allocation a test asks it to; it does nothing else.
 */
#include "doctest.h"
#include "ty_eval.h"

#include <stdlib.h>

#include <atomic>
#include <new>
#include <string>

namespace {

/* Allocations live, and the one to fail, counted down; -1 fails none. */
std::atomic<long> live{0};
std::atomic<long> fail_in{-1};

/*
 * An allocation of n bytes, or nullptr if it is the one to fail: fail_in
 * counts the allocations down to it, and stops at -1.
 */
void *counted_new(size_t n) {
  long left = fail_in.load();
  while (left >= 0 && !fail_in.compare_exchange_weak(left, left - 1)) {
  }
  if (0 == left) {
    return nullptr;
  }
  void *p = malloc(n ? n : 1);
  if (nullptr != p) {
    live++;
  }
  return p;
}

/* Free an allocation of counted_new. */
void counted_delete(void *p) {
  if (nullptr != p) {
    live--;
    free(p);
  }
}

} // namespace

void *operator new(size_t n) {
  void *p = counted_new(n);
  if (nullptr == p) {
    throw std::bad_alloc();
  }
  return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept {
  return counted_new(n);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
  return counted_new(n);
}
void operator delete(void *p) noexcept { counted_delete(p); }
void operator delete[](void *p) noexcept { counted_delete(p); }
void operator delete(void *p, size_t) noexcept { counted_delete(p); }
void operator delete[](void *p, size_t) noexcept { counted_delete(p); }

// Verify that the engine frees what it made when an allocation fails, and
// says TY_ERR_NOMEM.
// 1. For k from 0, fail the k-th allocation of ty_prog_compile, then of
//    ty_run_new, until one succeeds.
// 2. Each that fails: -1, TY_ERR_NOMEM, a NULL out, and as many
//    allocations live as before the call.
// 3. Both succeed after some, and are freed whole.
TEST_CASE("the engine frees what it made when memory runs out") {
  const std::string text =
      "(fn [x:i32.v k:i32] (let [y (add.i32.v x (splat.i32 k))]\n"
      "  (cmp.gt.i32.v y (splat.i32 (mul.i32 k 2)))))";
  // 1. and 2. The compile.
  ty_prog_t *p = nullptr;
  long k = 0;
  for (;; k++) {
    const long before = live.load();
    fail_in = k;
    const int rc = ty_prog_compile(text.data(), text.size(), &p);
    fail_in = -1;
    // Read before doctest's CAPTURE, which may allocate.
    const long after = live.load();
    if (0 == rc) {
      break;
    }
    CAPTURE(k);
    REQUIRE(nullptr == p);
    CHECK(TY_ERR_NOMEM == ty_errnum());
    CHECK(before == after);
  }
  CHECK(k > 5);
  // The run.
  ty_run_t *r = nullptr;
  long j = 0;
  for (;; j++) {
    const long before = live.load();
    fail_in = j;
    const int rc = ty_run_new(p, 2048, &r);
    fail_in = -1;
    // Read before doctest's CAPTURE, which may allocate.
    const long after = live.load();
    if (0 == rc) {
      break;
    }
    CAPTURE(j);
    REQUIRE(nullptr == r);
    CHECK(TY_ERR_NOMEM == ty_errnum());
    CHECK(before == after);
  }
  CHECK(j > 5);
  // 3. Freed whole.
  const long before = live.load();
  ty_run_free(r);
  ty_prog_free(p);
  CHECK(live.load() < before);
}
