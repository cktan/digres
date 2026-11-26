/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * eval_run.cc -- the machine of the evaluation engine (ty_eval.h,
 * ../EVAL_PLAN.md §3 and §5): a run of a program's bytecode on a stack of
 * slots, each a scalar or a vector of n rows, with its flags and errors.
 *
 * A vector slot's values are its own array, or the array of a parameter or
 * a local, which no operation writes: an operation writes its result into
 * the run's spare array, which then changes place with the slot's own, so
 * no operation's out is over an input. A slot's flags are always its own,
 * as an operation writes them over its first input's. A failing row is
 * marked (ty_vec.h); the run raises the lowest valid marked row at its end.
 */
#include <string.h>

#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "error.h"
#include "eval_prog.h"
#include "ty_bool.h"
#include "ty_eval.h"
#include "ty_float.h"
#include "ty_int.h"
#include "ty_vec.h"

using namespace ty_internal;

namespace {

/* One place of the stack, or a local. */
struct slot_t {
  bool vec = false;
  const void *values = nullptr; /* a vector's: own.data(), or a view */
  std::vector<uint64_t> own;    /* max_n values of 8 bytes at most */
  std::vector<uint8_t> flags;   /* a vector's: max_n */
  std::vector<ty_rowerr_t> err; /* a vector's errors */
  uint64_t scalar = 0;          /* a scalar's value, in its first bytes */
  uint8_t sflag = 0;            /* a scalar's flag */
  uint8_t scode = 0;            /* a scalar's error, if it failed */
};

/* A type of the language as its C type, for a dispatch. */
template <typename T> struct as_t {
  typedef T type;
};

/* Call f with the C type of t: int16_t to double, and uint8_t for bool. */
template <typename F> auto by_type(vtype_t t, F &&f) {
  switch (t) {
  case vtype_t::i16:
    return f(as_t<int16_t>{});
  case vtype_t::i32:
    return f(as_t<int32_t>{});
  case vtype_t::i64:
    return f(as_t<int64_t>{});
  case vtype_t::f32:
    return f(as_t<float>{});
  case vtype_t::f64:
    return f(as_t<double>{});
  case vtype_t::boolean:
  default:
    return f(as_t<uint8_t>{});
  }
}

/* A scalar slot's value as T. */
template <typename T> T get(const slot_t &s) {
  T v;
  memcpy(&v, &s.scalar, sizeof v);
  return v;
}

/* Set a scalar slot's value to v, of T. */
template <typename T> void put(slot_t &s, T v) {
  s.scalar = 0;
  memcpy(&s.scalar, &v, sizeof v);
}

/* The vector operations of two numbers, by op: ty_vec.h's. */
#define VEC_ARITH(T, TY)                                                       \
  int vec_arith(op_t op, uint32_t n, uint8_t *fa, const T *a,                  \
                const uint8_t *fb, const T *b, T *out,                         \
                std::vector<ty_rowerr_t> &err) {                               \
    switch (op) {                                                              \
    case op_t::add:                                                            \
      return tyvec_##TY##_add(n, fa, a, fb, b, out, err);                      \
    case op_t::sub:                                                            \
      return tyvec_##TY##_sub(n, fa, a, fb, b, out, err);                      \
    case op_t::mul:                                                            \
      return tyvec_##TY##_mul(n, fa, a, fb, b, out, err);                      \
    case op_t::div:                                                            \
      return tyvec_##TY##_div(n, fa, a, fb, b, out, err);                      \
    default:                                                                   \
      return vec_mod(n, fa, a, fb, b, out, err);                               \
    }                                                                          \
  }
#define VEC_MOD(T, TY)                                                         \
  int vec_mod(uint32_t n, uint8_t *fa, const T *a, const uint8_t *fb,          \
              const T *b, T *out, std::vector<ty_rowerr_t> &err) {             \
    return tyvec_##TY##_mod(n, fa, a, fb, b, out, err);                        \
  }
VEC_MOD(int16_t, int16)
VEC_MOD(int32_t, int32)
VEC_MOD(int64_t, int64)
/* Floats have no mod: the compiler takes none. */
int vec_mod(uint32_t, uint8_t *, const float *, const uint8_t *, const float *,
            float *, std::vector<ty_rowerr_t> &) {
  return 0;
}
int vec_mod(uint32_t, uint8_t *, const double *, const uint8_t *,
            const double *, double *, std::vector<ty_rowerr_t> &) {
  return 0;
}
VEC_ARITH(int16_t, int16)
VEC_ARITH(int32_t, int32)
VEC_ARITH(int64_t, int64)
VEC_ARITH(float, fp32)
VEC_ARITH(double, fp64)
/* bool has no arithmetic: the compiler takes none. */
int vec_arith(op_t, uint32_t, uint8_t *, const uint8_t *, const uint8_t *,
              const uint8_t *, uint8_t *, std::vector<ty_rowerr_t> &) {
  return 0;
}

/* The vector compares, by op: ty_vec.h's. */
#define VEC_CMP(T, TY)                                                         \
  void vec_cmp(op_t op, uint32_t n, uint8_t *f, const T *a, const T *b,        \
               uint8_t *out) {                                                 \
    switch (op) {                                                              \
    case op_t::eq:                                                             \
      tyvec_##TY##_eq(n, f, a, b, out);                                        \
      break;                                                                   \
    case op_t::ne:                                                             \
      tyvec_##TY##_ne(n, f, a, b, out);                                        \
      break;                                                                   \
    case op_t::lt:                                                             \
      tyvec_##TY##_lt(n, f, a, b, out);                                        \
      break;                                                                   \
    case op_t::le:                                                             \
      tyvec_##TY##_le(n, f, a, b, out);                                        \
      break;                                                                   \
    case op_t::gt:                                                             \
      tyvec_##TY##_gt(n, f, a, b, out);                                        \
      break;                                                                   \
    default:                                                                   \
      tyvec_##TY##_ge(n, f, a, b, out);                                        \
      break;                                                                   \
    }                                                                          \
  }
VEC_CMP(int16_t, int16)
VEC_CMP(int32_t, int32)
VEC_CMP(int64_t, int64)
VEC_CMP(float, fp32)
VEC_CMP(double, fp64)
VEC_CMP(uint8_t, bool)

/* The scalar operations of two numbers, by op: the error, or TY_OK. */
#define SCALAR_ARITH(T, TY)                                                    \
  ty_errnum_t scalar_arith(op_t op, T a, T b, T *r) {                          \
    int rc;                                                                    \
    switch (op) {                                                              \
    case op_t::add:                                                            \
      rc = ty_##TY##_add(a, b, r);                                             \
      break;                                                                   \
    case op_t::sub:                                                            \
      rc = ty_##TY##_sub(a, b, r);                                             \
      break;                                                                   \
    case op_t::mul:                                                            \
      rc = ty_##TY##_mul(a, b, r);                                             \
      break;                                                                   \
    case op_t::div:                                                            \
      rc = ty_##TY##_div(a, b, r);                                             \
      break;                                                                   \
    default:                                                                   \
      rc = scalar_mod(a, b, r);                                                \
      break;                                                                   \
    }                                                                          \
    return 0 == rc ? TY_OK : ty_errnum();                                      \
  }
int scalar_mod(int16_t a, int16_t b, int16_t *r) {
  return ty_int16_mod(a, b, r);
}
int scalar_mod(int32_t a, int32_t b, int32_t *r) {
  return ty_int32_mod(a, b, r);
}
int scalar_mod(int64_t a, int64_t b, int64_t *r) {
  return ty_int64_mod(a, b, r);
}
int scalar_mod(float, float, float *) { return 0; }
int scalar_mod(double, double, double *) { return 0; }
SCALAR_ARITH(int16_t, int16)
SCALAR_ARITH(int32_t, int32)
SCALAR_ARITH(int64_t, int64)
SCALAR_ARITH(float, fp32)
SCALAR_ARITH(double, fp64)
ty_errnum_t scalar_arith(op_t, uint8_t, uint8_t, uint8_t *) { return TY_OK; }

/* The scalar compares, by op. */
#define SCALAR_CMP(T, TY, C)                                                   \
  bool scalar_cmp(op_t op, T a, T b) {                                         \
    switch (op) {                                                              \
    case op_t::eq:                                                             \
      return ty_##TY##_eq(C(a), C(b));                                         \
    case op_t::ne:                                                             \
      return ty_##TY##_ne(C(a), C(b));                                         \
    case op_t::lt:                                                             \
      return ty_##TY##_lt(C(a), C(b));                                         \
    case op_t::le:                                                             \
      return ty_##TY##_le(C(a), C(b));                                         \
    case op_t::gt:                                                             \
      return ty_##TY##_gt(C(a), C(b));                                         \
    default:                                                                   \
      return ty_##TY##_ge(C(a), C(b));                                         \
    }                                                                          \
  }
SCALAR_CMP(int16_t, int16, )
SCALAR_CMP(int32_t, int32, )
SCALAR_CMP(int64_t, int64, )
SCALAR_CMP(float, fp32, )
SCALAR_CMP(double, fp64, )
SCALAR_CMP(uint8_t, bool, bool)

/*
 * A cast of v, of F, to T, into *r: TY_OK, or why it fails. Integers narrow
 * with a range check; a float to an integer rounds to the nearest, as
 * PostgreSQL's; an f64 to an f32 fails past its range; bool and i32 are 0 or
 * 1, and not 0.
 */
template <typename F, typename T> ty_errnum_t cast_one(F v, T *r) {
  if constexpr (std::is_same_v<F, uint8_t>) {
    *r = T(ty_bool_to_int32(0 != v));
    return TY_OK;
  } else if constexpr (std::is_same_v<T, uint8_t>) {
    *r = uint8_t(ty_bool_from_int32(int32_t(v)));
    return TY_OK;
  } else if constexpr (std::is_integral_v<F> && std::is_integral_v<T>) {
    if (v < std::numeric_limits<T>::min() ||
        v > std::numeric_limits<T>::max()) {
      return TY_ERR_RANGE;
    }
    *r = T(v);
    return TY_OK;
  } else if constexpr (std::is_integral_v<T>) {
    int rc;
    if constexpr (std::is_same_v<T, int16_t>) {
      rc = ty_fp64_to_int16(double(v), r);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      rc = ty_fp64_to_int32(double(v), r);
    } else {
      rc = ty_fp64_to_int64(double(v), r);
    }
    return 0 == rc ? TY_OK : ty_errnum();
  } else if constexpr (std::is_same_v<F, double> && std::is_same_v<T, float>) {
    return 0 == ty_fp64_to_fp32(v, r) ? TY_OK : ty_errnum();
  } else {
    *r = T(v);
    return TY_OK;
  }
}

/* Each row's first entry's code in err, if err has one; else none. */
std::vector<uint8_t> codes_of(uint32_t n, const std::vector<ty_rowerr_t> &err) {
  std::vector<uint8_t> c;
  if (!err.empty()) {
    c.assign(n, 0);
    for (const ty_rowerr_t &e : err) {
      if (e.row < n && 0 == c[e.row]) {
        c[e.row] = e.code;
      }
    }
  }
  return c;
}

} // namespace

/* A run's state (ty_eval.h). */
struct ty_run_t {
  const ty_prog_t *prog;
  uint32_t max_n;
  std::vector<slot_t> stack;
  std::vector<slot_t> locals;
  std::vector<uint64_t> spare;
  std::vector<uint8_t> spare_flags;
};

namespace {

/* A slot's arrays, for vectors of max_n rows. */
void size_slot(slot_t &s, uint32_t max_n) {
  s.own.assign(max_n ? max_n : 1, 0);
  s.flags.assign(max_n ? max_n : 1, 0);
}

/* Put the spare's values in s's place: the result of an operation on s. */
void take_spare(ty_run_t *r, slot_t &s) {
  std::swap(s.own, r->spare);
  s.values = s.own.data();
  s.vec = true;
}

/* Push a copy of src into dst, a vector's flags and errors its own. */
void copy_slot(slot_t &dst, const slot_t &src, uint32_t n) {
  dst.vec = src.vec;
  if (src.vec) {
    dst.values = src.values;
    memcpy(dst.flags.data(), src.flags.data(), n);
    dst.err = src.err;
  } else {
    dst.scalar = src.scalar;
    dst.sflag = src.sflag;
    dst.scode = src.scode;
  }
}

/*
 * A vector's select: each row from x where c is true, else from y, its
 * value, flags and error; a row c failed in is failed with c's error, and
 * one not valid in c not valid. Into c's slot, over c.
 */
template <typename T>
void select_vec(ty_run_t *r, slot_t &c, const slot_t &x, const slot_t &y,
                uint32_t n) {
  const uint8_t *cv = static_cast<const uint8_t *>(c.values);
  const T *xv = static_cast<const T *>(x.values);
  const T *yv = static_cast<const T *>(y.values);
  T *out = reinterpret_cast<T *>(r->spare.data());
  uint8_t *of = r->spare_flags.data();
  const std::vector<uint8_t> cc = codes_of(n, c.err), xc = codes_of(n, x.err),
                             yc = codes_of(n, y.err);
  std::vector<ty_rowerr_t> err;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t f = c.flags[i];
    uint8_t code = 0;
    if (f & TY_VEC_NOTVALID) {
      of[i] = TY_VEC_NOTVALID;
      out[i] = T(0);
    } else if (f & TY_VEC_ERROR) {
      of[i] = TY_VEC_ERROR;
      out[i] = T(0);
      code = cc.empty() ? 0 : cc[i];
    } else if (0 == (f & TY_VEC_NULL) && 0 != cv[i]) {
      of[i] = x.flags[i];
      out[i] = xv[i];
      code = (x.flags[i] & TY_VEC_ERROR) && !xc.empty() ? xc[i] : 0;
    } else {
      of[i] = y.flags[i];
      out[i] = yv[i];
      code = (y.flags[i] & TY_VEC_ERROR) && !yc.empty() ? yc[i] : 0;
    }
    if ((of[i] & TY_VEC_ERROR) && 0 != code) {
      err.push_back({i, code});
    }
  }
  take_spare(r, c);
  std::swap(c.flags, r->spare_flags);
  c.err = std::move(err);
}

/*
 * A vector's cast, row by row, of each row computed; a row that fails is
 * marked. Into s's slot.
 */
template <typename F, typename T>
void cast_vec(ty_run_t *r, slot_t &s, uint32_t n) {
  const F *v = static_cast<const F *>(s.values);
  T *out = reinterpret_cast<T *>(r->spare.data());
  for (uint32_t i = 0; i < n; i++) {
    out[i] = T(0);
    if (0 == (s.flags[i] & TY_VEC_SKIP)) {
      const ty_errnum_t why = cast_one<F, T>(v[i], &out[i]);
      if (TY_OK != why) {
        s.flags[i] |= TY_VEC_ERROR;
        s.err.push_back({i, uint8_t(why)});
      }
    }
  }
  take_spare(r, s);
}

/* A scalar's cast, into s. */
template <typename F, typename T> void cast_scalar(slot_t &s) {
  if (0 == (s.sflag & (TY_VEC_NULL | TY_VEC_ERROR))) {
    T out = T(0);
    const ty_errnum_t why = cast_one<F, T>(get<F>(s), &out);
    if (TY_OK != why) {
      s.sflag |= TY_VEC_ERROR;
      s.scode = uint8_t(why);
    } else {
      put<T>(s, out);
    }
  }
}

/*
 * The flag and error of a scalar of two operands: a's error first, then
 * b's, then null if one is. False if the operation is to compute.
 */
bool scalar_skip(slot_t &a, const slot_t &b) {
  if (a.sflag & TY_VEC_ERROR) {
    return true;
  }
  if (b.sflag & TY_VEC_ERROR) {
    a.sflag = TY_VEC_ERROR;
    a.scode = b.scode;
    return true;
  }
  if ((a.sflag | b.sflag) & TY_VEC_NULL) {
    a.sflag = TY_VEC_NULL;
    return true;
  }
  return false;
}

/* Fail with a scalar's error. */
int raise_scalar(const slot_t &s) {
  const uint8_t f = TY_VEC_ERROR;
  const std::vector<ty_rowerr_t> err = {{0, s.scode}};
  return tyvec_flag_error(1, &f, err);
}

/*
 * Run the program on n rows.
 * 1. Each instruction in turn, on the stack's top slots; a jump sets pc.
 * 2. ret: raise the lowest valid failed row, or give the result.
 */
int run(ty_run_t *r, uint32_t n, const ty_arg_t *args, ty_result_t *result) {
  const ty_prog_t *p = r->prog;
  std::vector<slot_t> &st = r->stack;
  size_t sp = 0; /* the slots in use */
  for (size_t pc = 0; pc < p->code.size();) {
    const instr_t &in = p->code[pc++];
    switch (in.op) {
    case op_t::load: {
      slot_t &s = st[sp++];
      const ty_arg_t &a = args[in.a];
      s.vec = in.vec;
      s.err.clear();
      if (in.vec) {
        s.values = a.values;
        if (nullptr != a.flags) {
          memcpy(s.flags.data(), a.flags, n);
        } else {
          memset(s.flags.data(), 0, n);
        }
      } else {
        s.scalar = 0;
        memcpy(&s.scalar, a.values, vtype_size(in.type));
        s.sflag = nullptr != a.flags ? a.flags[0] : 0;
        s.scode = 0;
      }
      break;
    }
    case op_t::local_set:
      std::swap(r->locals[in.a], st[--sp]);
      break;
    case op_t::local_get:
      copy_slot(st[sp++], r->locals[in.a], n);
      break;
    case op_t::konst: {
      slot_t &s = st[sp++];
      const konst_t &k = p->consts[in.a];
      s.vec = false;
      s.sflag = k.null ? TY_VEC_NULL : 0;
      s.scode = 0;
      s.scalar = 0;
      by_type(k.type, [&](auto t) {
        using T = typename decltype(t)::type;
        if constexpr (std::is_same_v<T, float>) {
          put<T>(s, k.f);
        } else if constexpr (std::is_same_v<T, double>) {
          put<T>(s, k.d);
        } else {
          put<T>(s, T(k.i));
        }
      });
      break;
    }
    case op_t::splat: {
      slot_t &s = st[sp - 1];
      by_type(in.type, [&](auto t) {
        using T = typename decltype(t)::type;
        const T v = get<T>(s);
        T *out = reinterpret_cast<T *>(r->spare.data());
        for (uint32_t i = 0; i < n; i++) {
          out[i] = v;
        }
      });
      const uint8_t f = s.sflag & (TY_VEC_NULL | TY_VEC_ERROR);
      memset(s.flags.data(), f, n);
      s.err.clear();
      for (uint32_t i = 0; (f & TY_VEC_ERROR) && i < n; i++) {
        s.err.push_back({i, s.scode});
      }
      take_spare(r, s);
      break;
    }
    case op_t::add:
    case op_t::sub:
    case op_t::mul:
    case op_t::div:
    case op_t::mod: {
      slot_t &a = st[sp - 2], &b = st[sp - 1];
      by_type(in.type, [&](auto t) {
        using T = typename decltype(t)::type;
        if (in.vec) {
          vec_arith(in.op, n, a.flags.data(), static_cast<const T *>(a.values),
                    b.flags.data(), static_cast<const T *>(b.values),
                    reinterpret_cast<T *>(r->spare.data()), a.err);
          tyvec_err_merge(n, a.flags.data(), a.err, b.err);
          take_spare(r, a);
        } else if (!scalar_skip(a, b)) {
          T out = T(0);
          const ty_errnum_t why =
              scalar_arith(in.op, get<T>(a), get<T>(b), &out);
          if (TY_OK != why) {
            a.sflag = TY_VEC_ERROR;
            a.scode = uint8_t(why);
          } else {
            put<T>(a, out);
          }
        }
      });
      sp--;
      break;
    }
    case op_t::eq:
    case op_t::ne:
    case op_t::lt:
    case op_t::le:
    case op_t::gt:
    case op_t::ge: {
      slot_t &a = st[sp - 2], &b = st[sp - 1];
      by_type(in.type, [&](auto t) {
        using T = typename decltype(t)::type;
        if (in.vec) {
          ty_bitor_vec(n, a.flags.data(), b.flags.data(), nullptr);
          vec_cmp(in.op, n, a.flags.data(), static_cast<const T *>(a.values),
                  static_cast<const T *>(b.values),
                  reinterpret_cast<uint8_t *>(r->spare.data()));
          tyvec_err_merge(n, a.flags.data(), a.err, b.err);
          take_spare(r, a);
        } else if (!scalar_skip(a, b)) {
          put<uint8_t>(a, scalar_cmp(in.op, get<T>(a), get<T>(b)) ? 1 : 0);
        }
      });
      sp--;
      break;
    }
    case op_t::bnot: {
      slot_t &a = st[sp - 1];
      if (in.vec) {
        tyvec_bool_not(n, a.flags.data(),
                       static_cast<const uint8_t *>(a.values),
                       reinterpret_cast<uint8_t *>(r->spare.data()));
        take_spare(r, a);
      } else if (0 == (a.sflag & (TY_VEC_NULL | TY_VEC_ERROR))) {
        put<uint8_t>(a, 0 == get<uint8_t>(a));
      }
      break;
    }
    case op_t::band:
    case op_t::bor: {
      slot_t &a = st[sp - 2], &b = st[sp - 1];
      const bool is_and = op_t::band == in.op;
      if (in.vec) {
        const uint8_t *x = static_cast<const uint8_t *>(a.values);
        const uint8_t *y = static_cast<const uint8_t *>(b.values);
        uint8_t *out = reinterpret_cast<uint8_t *>(r->spare.data());
        if (is_and) {
          tyvec_bool_and(n, a.flags.data(), x, b.flags.data(), y, out, a.err);
        } else {
          tyvec_bool_or(n, a.flags.data(), x, b.flags.data(), y, out, a.err);
        }
        tyvec_err_merge(n, a.flags.data(), a.err, b.err);
        take_spare(r, a);
      } else if (0 == (a.sflag & TY_VEC_ERROR)) {
        // a is not the side that settles it, as br_and or br_or jumped
        // past then: b decides, but a null a and a b that does not settle.
        if (b.sflag & TY_VEC_ERROR) {
          a.sflag = TY_VEC_ERROR;
          a.scode = b.scode;
        } else if (a.sflag & TY_VEC_NULL) {
          const bool settles =
              0 == (b.sflag & TY_VEC_NULL) && (0 != get<uint8_t>(b)) == !is_and;
          if (settles) {
            a.sflag = 0;
            put<uint8_t>(a, get<uint8_t>(b));
          }
        } else {
          a.sflag = b.sflag;
          put<uint8_t>(a, get<uint8_t>(b));
        }
      }
      sp--;
      break;
    }
    case op_t::isnull:
    case op_t::isnotnull:
    case op_t::istrue:
    case op_t::isfalse: {
      slot_t &a = st[sp - 1];
      if (in.vec) {
        uint8_t *out = reinterpret_cast<uint8_t *>(r->spare.data());
        const uint8_t *v = static_cast<const uint8_t *>(a.values);
        switch (in.op) {
        case op_t::isnull:
          tyvec_flag_isnull(n, a.flags.data(), out);
          break;
        case op_t::isnotnull:
          tyvec_flag_isnotnull(n, a.flags.data(), out);
          break;
        case op_t::istrue:
          tyvec_bool_istrue(n, a.flags.data(), v, out);
          break;
        default:
          tyvec_bool_isfalse(n, a.flags.data(), v, out);
          break;
        }
        take_spare(r, a);
      } else if (0 == (a.sflag & TY_VEC_ERROR)) {
        const bool null = a.sflag & TY_VEC_NULL;
        const bool v = !null && op_t::istrue <= in.op && 0 != get<uint8_t>(a);
        bool out = op_t::isnull == in.op      ? null
                   : op_t::isnotnull == in.op ? !null
                   : op_t::istrue == in.op    ? v
                                              : !null && !v;
        a.sflag = 0;
        put<uint8_t>(a, out);
      }
      break;
    }
    case op_t::cast: {
      slot_t &a = st[sp - 1];
      by_type(in.type, [&](auto f) {
        using F = typename decltype(f)::type;
        by_type(in.to, [&](auto t) {
          using T = typename decltype(t)::type;
          if constexpr (!std::is_same_v<F, T>) {
            if (in.vec) {
              cast_vec<F, T>(r, a, n);
            } else {
              cast_scalar<F, T>(a);
            }
          }
        });
      });
      break;
    }
    case op_t::select: {
      slot_t &c = st[sp - 3];
      by_type(in.type, [&](auto t) {
        using T = typename decltype(t)::type;
        select_vec<T>(r, c, st[sp - 2], st[sp - 1], n);
      });
      sp -= 2;
      break;
    }
    case op_t::br:
      pc = in.a;
      break;
    case op_t::br_ifnot: {
      const slot_t &c = st[--sp];
      if (c.sflag & TY_VEC_ERROR) {
        slot_t &s = st[sp++];
        const uint8_t code = c.scode;
        s.vec = false;
        s.scalar = 0;
        s.sflag = TY_VEC_ERROR;
        s.scode = code;
        pc = in.b;
      } else if ((c.sflag & TY_VEC_NULL) || 0 == get<uint8_t>(c)) {
        pc = in.a;
      }
      break;
    }
    case op_t::br_and:
    case op_t::br_or: {
      const slot_t &a = st[sp - 1];
      const bool known = 0 == (a.sflag & (TY_VEC_NULL | TY_VEC_ERROR));
      if (known && (0 != get<uint8_t>(a)) == (op_t::br_or == in.op)) {
        pc = in.a;
      }
      break;
    }
    case op_t::ret: {
      // 2. The result.
      slot_t &s = st[sp - 1];
      if (s.vec) {
        if (0 != tyvec_flag_error(n, s.flags.data(), s.err)) {
          return -1;
        }
        result->values = s.values;
        result->flags = s.flags.data();
      } else {
        if (s.sflag & TY_VEC_ERROR) {
          return raise_scalar(s);
        }
        result->values = &s.scalar;
        result->flags = &s.sflag;
      }
      return 0;
    }
    }
  }
  return set_error(TY_ERR_INVALID, "a program with no ret");
}

} // namespace

/* A thread's state for a program; see ty_eval.h. */
int ty_run_new(const ty_prog_t *prog, uint32_t max_n, ty_run_t **out) {
  *out = nullptr;
  try {
    ty_run_t *r = new ty_run_t;
    r->prog = prog;
    r->max_n = max_n;
    r->stack.resize(prog->max_depth);
    r->locals.resize(prog->locals.size());
    for (slot_t &s : r->stack) {
      size_slot(s, max_n);
    }
    for (slot_t &s : r->locals) {
      size_slot(s, max_n);
    }
    r->spare.assign(max_n ? max_n : 1, 0);
    r->spare_flags.assign(max_n ? max_n : 1, 0);
    *out = r;
    return 0;
  } catch (const std::bad_alloc &) {
    return set_error(TY_ERR_NOMEM, "out of memory");
  }
}

/* Free a run; see ty_eval.h. */
void ty_run_free(ty_run_t *run) { delete run; }

/* Run on n rows; see ty_eval.h. */
int ty_run(ty_run_t *run, uint32_t n, const ty_arg_t *args,
           ty_result_t *result) {
  if (n > run->max_n) {
    return set_error(TY_ERR_INVALID, "more rows than the run was made for");
  }
  try {
    return ::run(run, n, args, result);
  } catch (const std::bad_alloc &) {
    return set_error(TY_ERR_NOMEM, "out of memory");
  }
}
