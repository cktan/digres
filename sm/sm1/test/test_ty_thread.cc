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

#include <string>
#include <thread>

#include "doctest.h"
#include "sm0_arena.h"
#include "ty.h"
#include "ty_text.h"

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
