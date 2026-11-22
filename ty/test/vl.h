/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * vl.h -- helpers of the tests for the values that are varlenas: make one
 * from a string, and read the result of a function that makes one.
 */
#ifndef TY_TEST_VL_H
#define TY_TEST_VL_H

#include <string.h>

#include <string>
#include <vector>

#include "ty_varlena.h"

/*
 * A varlena of the bytes of a string. The header is short if the value has
 * at most 126 bytes, unless long_hdr is set, and long if it has more: the two
 * forms that the host may give.
 */
struct vl_t {
  std::vector<char> b;

  explicit vl_t(const std::string &s, bool long_hdr = false) {
    if (s.size() <= 126 && !long_hdr) {
      b.resize(s.size() + 1);
      b[0] = (char)(((s.size() + 1) << 1) | 1);
      memcpy(b.data() + 1, s.data(), s.size());
    } else {
      uint32_t h = TY_VL_LONG_HEADER(s.size() + 4);

      b.resize(s.size() + 4);
      memcpy(b.data(), &h, 4);
      memcpy(b.data() + 4, s.data(), s.size());
    }
  }
  operator const ty_varlena_t *() const {
    return (const ty_varlena_t *)b.data();
  }
};

/* The bytes of a varlena, as a string. */
inline std::string vl_str(const ty_varlena_t *v) {
  return std::string(ty_vl_data(v), ty_vl_len(v));
}

/*
 * The result of a function that makes a varlena. The destructor frees it,
 * unless it is the input itself, which the function may give back.
 */
struct vl_out_t {
  ty_varlena_t *p = nullptr;
  const ty_varlena_t *input;

  explicit vl_out_t(const ty_varlena_t *in = nullptr) : input(in) {}
  vl_out_t(const vl_out_t &) = delete;
  ~vl_out_t() {
    if (p != nullptr && p != input) {
      ty_free(p);
    }
  }
  ty_varlena_t **operator&() { return &p; }
  std::string str() const { return vl_str(p); }
};

#endif
