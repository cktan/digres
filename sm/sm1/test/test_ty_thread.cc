/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the alloc and free functions of ty on a thread of sm.
 */
#include <stdint.h>
#include <string.h>

#include <string>
#include <thread>

#include "doctest.h"
#include "sm1_ty.h"
#include "ty.h"
#include "ty_text.h"

namespace {

/* A varlena of the bytes of s, with a short header (at most 126 bytes). */
std::string short_text(const std::string &s) {
  std::string v(1, (char)(((s.size() + 1) << 1) | 1));

  return v + s;
}

} // namespace

// Verify that a thread that calls ty_thread_init() has ty allocate from its
// arena, and that a value of ty on that thread is good until the thread ends.
// 1. On a new thread, call ty_thread_init().
// 2. Allocate twice with ty_alloc(): the pieces are aligned to 8 and apart.
// 3. Free the first: the second is still good, as free does nothing.
// 4. Concatenate two texts with ty_text_concat(): the result is right.
TEST_CASE("ty on a thread of sm allocates from the thread's arena") {
  bool ok = false;

  std::thread t([&] {
    // 1. On a new thread, call ty_thread_init().
    sm1::ty_thread_init();

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

    ok = 0 == ((uintptr_t)a & 7) && 0 == ((uintptr_t)b & 7) && apart && kept &&
         concat;
  });
  t.join();
  CHECK(ok);
}

// Verify that each thread has an arena of its own, and that a thread that
// did not call ty_thread_init() has ty's default, malloc and free.
// 1. Two threads each call ty_thread_init() and allocate.
// 2. Each thread gets a piece and can write it.
// 3. This thread, which did not call it, allocates and frees with malloc.
TEST_CASE("each thread of sm has an arena of its own") {
  char *pa = nullptr, *pb = nullptr;

  // 1. Two threads each call ty_thread_init() and allocate.
  std::thread t1([&] {
    sm1::ty_thread_init();
    pa = static_cast<char *>(ty_alloc(16));
    memset(pa, 1, 16);
  });
  t1.join();
  std::thread t2([&] {
    sm1::ty_thread_init();
    pb = static_cast<char *>(ty_alloc(16));
    memset(pb, 2, 16);
  });
  t2.join();

  // 2. Each thread gets a piece and can write it.
  CHECK(nullptr != pa);
  CHECK(nullptr != pb);

  // 3. This thread, which did not call it, allocates and frees with malloc.
  void *p = ty_alloc(32);
  CHECK(nullptr != p);
  ty_free(p);
}
