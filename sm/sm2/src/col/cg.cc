/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * cg.cc -- the Column Store's column group (../COLSTORE.md §5, §6):
 * its encodings, its file, its zonemap, its filter, and the
 * pruning test.
 *
 * A CG file, in order:
 *
 *   body     zstd of the encoded values (below)
 *   stats    cg_stats_encode's bytes
 *   filter   the filter key's value
 *   tail     fixed: magic, format, encoding, rows, the sizes of
 *            the parts above, the CRC32C of all before it, magic
 *
 * The body, before zstd: the encoding byte, the row count, a byte
 * that says whether flag bytes follow, a flag byte a row if a row is
 * null, then the values as the encoding lays them out. Every value is
 * in the body: a column store table takes no varlena past its limit
 * (sm3::col_max_value).
 *
 * A CG is decoded into arrays (cg_vec, ../../COLDECODE_PLAN.md).
 */

#include "sm2.h"

#include "collation.h"
#include "native.h"
#include "sm0_codec.h"
#include "sm0_crc.h"
#include "ty_vec.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_set>

namespace {

/* The encodings (../COLSTORE.md §5.1). */
enum : uint8_t {
  enc_plain = 1, /* each value, in row order */
  enc_dict = 2,  /* a sorted dictionary, and bit-packed codes */
  enc_delta = 3, /* sorted integers: a base, and bit-packed deltas */
};

/* The filter's kinds (../COLSTORE.md §6.3). */
enum : uint8_t {
  filter_none = 0,  /* no value to filter */
  filter_exact = 1, /* the distinct keys, sorted */
  filter_bloom = 2, /* a bloom filter of the distinct keys */
};

/* A filter is an exact set while its keys total this or less. */
constexpr size_t exact_max = 4096;
/* A bloom filter's bits for each distinct key, and its probes. */
constexpr uint32_t bloom_bits_per_key = 10;
constexpr uint8_t bloom_probes = 7;

/* The tail's magic, format, and size. */
constexpr uint32_t cg_magic = 0x53434731; /* "SCG1" */
constexpr uint32_t cg_format = 2;
/* magic, format, encoding, flags, rows, 4 sizes, CRC32C, magic. */
constexpr size_t tail_size = 4 + 4 + 1 + 1 + 4 + 4 * 4 + 4 + 4;

/*
 * A null row's flag byte in the body; 0 is a row not null. It is ty's
 * bit, so that a flag byte is read as it is, and part of the format.
 */
constexpr uint8_t flag_null = 2;
static_assert(flag_null == TY_VEC_NULL);

/* The spare bytes past a body: unpack's last load stays inside them. */
constexpr size_t body_spare = 8;

/* The stats' flag bits. */
enum : uint8_t {
  st_min_cut = 0x01,
  st_max_cut = 0x02,
  st_nan = 0x04,
  st_pos_inf = 0x08,
  st_neg_inf = 0x10,
  st_sorted = 0x40,
};

/* Fail with SM_ERR_CORRUPT: what is wrong with a CG's bytes. */
int corrupt(const char *what) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, std::string("CG: ") + what);
}

/* Append a 32-bit length, then the bytes of s. */
void put_str(std::string &out, std::string_view s) {
  sm0::put_be32(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

/* Read put_str's bytes from the front of in; false if cut short. */
bool get_str(std::string_view &in, std::string &s) {
  if (in.size() < 4) {
    return false;
  }
  uint32_t n = sm0::get_be32(in.data());
  if (in.size() - 4 < n) {
    return false;
  }
  s.assign(in.data() + 4, n);
  in.remove_prefix(4 + n);
  return true;
}

/* Read a big-endian integer of 4 bytes from the front of in. */
bool get_u32(std::string_view &in, uint32_t &v) {
  if (in.size() < 4) {
    return false;
  }
  v = sm0::get_be32(in.data());
  in.remove_prefix(4);
  return true;
}

/* The bits a value up to max needs: 0 for max 0. */
uint8_t bits_for(uint64_t max) {
  return max == 0 ? 0 : static_cast<uint8_t>(64 - std::countl_zero(max));
}

/* Append n values of width bits each, packed low bit first. */
void pack(std::string &out, const std::vector<uint64_t> &v, uint8_t bits) {
  size_t start = out.size();
  out.append((v.size() * bits + 7) / 8, '\0');
  unsigned char *p = reinterpret_cast<unsigned char *>(out.data() + start);
  size_t bit = 0;
  for (uint64_t x : v) {
    for (uint8_t b = 0; b < bits; b++, bit++) {
      if ((x >> b) & 1) {
        p[bit / 8] |= static_cast<unsigned char>(1u << (bit % 8));
      }
    }
  }
}

/*
 * Read n values of bits bits each from the front of in, low bit first,
 * into out, whose type is as wide: false if they are cut short. Past
 * in's end are body_spare bytes.
 * 1. A value of up to 56 bits is in the 8 bytes from its first bit's.
 * 2. A wider one is read a bit at a time.
 */
template <typename T>
bool unpack(std::string_view &in, size_t n, uint8_t bits, T *out) {
  static_assert(8 <= body_spare);
  if (bits > 8 * sizeof(T)) {
    return false;
  }
  const size_t bytes = (n * bits + 7) / 8;
  if (in.size() < bytes) {
    return false;
  }
  const char *p = in.data();
  in.remove_prefix(bytes);
  // 1. One load a value.
  if (bits <= 56) {
    const uint64_t mask = (uint64_t(1) << bits) - 1;
    size_t bit = 0;
    for (size_t i = 0; i < n; i++, bit += bits) {
      uint64_t x = sm2::load_le<uint64_t>({p + bit / 8, 8});
      out[i] = static_cast<T>((x >> (bit % 8)) & mask);
    }
    return true;
  }
  // 2. A bit at a time.
  size_t bit = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t x = 0;
    for (uint8_t b = 0; b < bits; b++, bit++) {
      if ((static_cast<unsigned char>(p[bit / 8]) >> (bit % 8)) & 1) {
        x |= uint64_t(1) << b;
      }
    }
    out[i] = static_cast<T>(x);
  }
  return true;
}

/*
 * Read n flag bytes from the front of in into out: false if they are cut
 * short, or one is not a flag byte.
 */
bool get_flags(std::string_view &in, size_t n, std::vector<uint8_t> &out) {
  if (in.size() < n) {
    return false;
  }
  out.resize(n);
  uint8_t bad = 0;
  for (size_t i = 0; i < n; i++) {
    out[i] = static_cast<uint8_t>(in[i]);
    bad |= out[i] & static_cast<uint8_t>(~flag_null);
  }
  in.remove_prefix(n);
  return 0 == bad;
}

/*
 * A 64-bit hash of key bytes, for the bloom filter: FNV-1a, then the
 * finalizer of splitmix64 to spread its bits. It is part of the file
 * format: it must never change.
 */
uint64_t key_hash(std::string_view k) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : k) {
    h = (h ^ c) * 0x100000001b3ull;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

/* The bit of probe i of hash h in a filter of nbits bits. */
uint32_t probe_bit(uint64_t h, uint8_t i, uint32_t nbits) {
  uint64_t h1 = h, h2 = (h >> 32) | (h << 32);
  return static_cast<uint32_t>((h1 + i * h2) % nbits);
}

/* The keys of a type's NaN and infinities, empty where it has none. */
struct specials_t {
  std::string nan, pos_inf, neg_inf;
};

/* Key the datum of text t, if the type takes it; else leave k empty. */
void special_key(const sm2::cg_column_t &col, std::string_view t,
                 std::string &k) {
  std::string d;
  // A type with no such value refuses the text: not an error here.
  if (col.type->in(t, col.typmod, d) != 0 ||
      col.type->key_encode(d, col.typmod, col.collation, k) != 0) {
    k.clear();
    sm1::reseterr();
  }
}

/*
 * Whether t has NaN or an infinity, values that sort outside its
 * finite ones (../../COLSTORE.md §6.1). A string type is not one: its
 * in takes any text, 'NaN' as any other.
 */
bool has_specials(const sm2::type_t *t) {
  return t == &sm2::type_float4 || t == &sm2::type_float8 ||
         t == &sm2::type_date || t == &sm2::type_timestamp ||
         t == &sm2::type_timestamptz || t == &sm2::type_interval;
}

/* The special keys of col's type: by its text, as PostgreSQL's. */
specials_t specials_of(const sm2::cg_column_t &col) {
  specials_t s;
  // A type with none keeps every value in min and max.
  if (!has_specials(col.type)) {
    return s;
  }
  special_key(col, "NaN", s.nan);
  special_key(col, "Infinity", s.pos_inf);
  special_key(col, "-Infinity", s.neg_inf);
  if (s.pos_inf.empty()) {
    special_key(col, "infinity", s.pos_inf);
  }
  if (s.neg_inf.empty()) {
    special_key(col, "-infinity", s.neg_inf);
  }
  return s;
}

/* Whether the type's datums are integers that delta can encode. */
bool is_int(const sm2::type_t *t) {
  return t == &sm2::type_int2 || t == &sm2::type_int4 || t == &sm2::type_int8;
}

/* An integer datum of width bytes, little-endian, as int64. */
int64_t int_of(std::string_view d) {
  if (d.size() == 2) {
    int16_t v;
    memcpy(&v, d.data(), 2);
    return v;
  }
  if (d.size() == 4) {
    int32_t v;
    memcpy(&v, d.data(), 4);
    return v;
  }
  int64_t v;
  memcpy(&v, d.data(), 8);
  return v;
}

/* Whether some value of st may be above k (or at it, with incl). */
bool may_be_above(const sm2::cg_stats_t &st, const specials_t &sp,
                  std::string_view k, bool incl) {
  // The greatest value present: NaN, +infinity, then the max.
  std::string_view top;
  bool cut = false;
  if (st.has_nan && !sp.nan.empty()) {
    top = sp.nan;
  } else if (st.has_pos_inf && !sp.pos_inf.empty()) {
    top = sp.pos_inf;
  } else if (!st.max.empty()) {
    top = st.max;
    cut = st.max_cut;
  } else if (st.has_neg_inf && !sp.neg_inf.empty()) {
    top = sp.neg_inf;
  } else {
    return false;
  }
  if (cut) {
    return k.substr(0, top.size()).compare(top) <= 0;
  }
  int c = top.compare(k);
  return incl ? c >= 0 : c > 0;
}

/* Whether some value of st may be below k (or at it, with incl). */
bool may_be_below(const sm2::cg_stats_t &st, const specials_t &sp,
                  std::string_view k, bool incl) {
  // The least value present: -infinity, then the min, then the
  // values above every finite one.
  std::string_view low;
  if (st.has_neg_inf && !sp.neg_inf.empty()) {
    low = sp.neg_inf;
  } else if (!st.min.empty()) {
    low = st.min; // a cut min is still a lower bound
  } else if (st.has_pos_inf && !sp.pos_inf.empty()) {
    low = sp.pos_inf;
  } else if (st.has_nan && !sp.nan.empty()) {
    low = sp.nan;
  } else {
    return false;
  }
  int c = low.compare(k);
  return incl ? c <= 0 : c < 0;
}

/* Whether the filter may hold key k. */
bool filter_may_hold(const sm2::cg_column_t &col, const sm2::cg_stats_t &st,
                     std::string_view filter, std::string_view k) {
  if (filter.empty()) {
    return true;
  }
  std::string_view in = filter;
  uint8_t kind = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  if (kind == filter_none) {
    return false;
  }
  if (kind == filter_exact) {
    uint32_t n;
    if (!get_u32(in, n)) {
      return true;
    }
    for (uint32_t i = 0; i < n; i++) {
      std::string e;
      if (!get_str(in, e)) {
        return true;
      }
      int c = std::string_view(e).compare(k);
      if (c == 0) {
        return true;
      }
      // Sorted: past k, k is not there.
      if (c > 0) {
        return false;
      }
    }
    return false;
  }
  if (kind == filter_bloom) {
    if (in.size() < 5) {
      return true;
    }
    uint8_t probes = static_cast<uint8_t>(in[0]);
    uint32_t nbits = sm0::get_be32(in.data() + 1);
    in.remove_prefix(5);
    if (nbits == 0 || in.size() < (nbits + 7) / 8) {
      return true;
    }
    uint64_t h = key_hash(k);
    for (uint8_t i = 0; i < probes; i++) {
      uint32_t b = probe_bit(h, i, nbits);
      if (!((in[b / 8] >> (b % 8)) & 1)) {
        return false;
      }
    }
    return true;
  }
  return true;
}

} // namespace

/*
 * A CG decoded into arrays (cg_vec_t). The values are a fixed type's bytes
 * in fixed, or a varlena type's addresses in ptrs; those point into body,
 * or at no_varlena for a null row.
 */
struct sm2::cg_t {
  const type_t *type = nullptr;
  uint32_t n = 0;                /* rows */
  uint32_t nvalues = 0;          /* values: n, or the dictionary's */
  bool dict = false;             /* codes are the rows */
  std::unique_ptr<char[]> fixed; /* nvalues of type->len bytes */
  std::vector<const char *> ptrs;
  std::vector<uint32_t> codes;
  std::vector<uint8_t> flags; /* empty: no row is null */
  std::string body;           /* empty for a fixed type */
};

namespace {

/* What a null row of a varlena type points at: an empty varlena. */
constexpr char no_varlena[1] = {'\x03'};

/*
 * Whether v is one whole varlena by its header, neither compressed nor a
 * TOAST pointer, whose header says a length of 0.
 */
bool is_varlena(std::string_view v) {
  if (v.empty()) {
    return false;
  }
  const uint8_t first = static_cast<uint8_t>(v[0]);
  if (first & 1) {
    return v.size() == first >> 1;
  }
  if (v.size() < 4) {
    return false;
  }
  const uint32_t header = sm2::load_le<uint32_t>(v.substr(0, 4));
  return 0 == (header & 3) && v.size() == header >> 2;
}

/* The whole varlena at p, which is_varlena said yes to. */
std::string_view varlena_at(const char *p) {
  const uint8_t first = static_cast<uint8_t>(*p);
  const size_t len =
      first & 1 ? first >> 1 : sm2::load_le<uint32_t>({p, 4}) >> 2;
  return {p, len};
}

/*
 * Read n values of W bytes each, put_str's, from the front of in into out;
 * a row flags has as null, if it is not nullptr, is of length 0, and is 0
 * there. Returns false if one is cut short, or of another length.
 */
template <size_t W>
bool get_fixed(std::string_view &in, size_t n, const uint8_t *flags,
               char *out) {
  const char *p = in.data(), *end = p + in.size();
  for (size_t i = 0; i < n; i++, out += W) {
    const size_t want = flags && flags[i] ? 0 : W;
    if (static_cast<size_t>(end - p) < 4 + want || sm0::get_be32(p) != want) {
      return false;
    }
    p += 4;
    if (0 == want) {
      memset(out, 0, W);
    } else {
      memcpy(out, p, W);
      p += W;
    }
  }
  in.remove_prefix(p - in.data());
  return true;
}

/* get_fixed of a type of len bytes. */
bool get_fixed(std::string_view &in, size_t n, const uint8_t *flags, size_t len,
               char *out) {
  switch (len) {
  case 1:
    return get_fixed<1>(in, n, flags, out);
  case 2:
    return get_fixed<2>(in, n, flags, out);
  case 4:
    return get_fixed<4>(in, n, flags, out);
  case 8:
    return get_fixed<8>(in, n, flags, out);
  case 16:
    return get_fixed<16>(in, n, flags, out);
  default: // name
    return 64 == len && get_fixed<64>(in, n, flags, out);
  }
}

/*
 * Read n varlenas, put_str's, from the front of the body's in, and their
 * addresses into out. Returns false if one is cut short, or is no varlena.
 * 1. Its length; a null row's is 0.
 * 2. The varlena, whose header must say that length.
 * 3. One with a 4-byte header is aligned to 4, as PostgreSQL's macros need:
 *    it is moved left, over the length just read.
 */
bool get_varlenas(std::string_view &in, size_t n, const uint8_t *flags,
                  std::vector<const char *> &out) {
  // The bytes are the body's own, which the caller may write.
  char *p = const_cast<char *>(in.data());
  const char *end = p + in.size();
  out.resize(n);
  for (size_t i = 0; i < n; i++) {
    // 1. Its length.
    if (end - p < 4) {
      return false;
    }
    const size_t len = sm0::get_be32(p);
    p += 4;
    if (static_cast<size_t>(end - p) < len) {
      return false;
    }
    if (flags && flags[i]) {
      if (0 != len) {
        return false;
      }
      out[i] = no_varlena;
      continue;
    }
    // 2. The varlena.
    if (!is_varlena({p, len})) {
      return false;
    }
    // 3. Its alignment.
    char *at = p;
    const size_t off = reinterpret_cast<uintptr_t>(p) % 4;
    if (0 == (*p & 1) && 0 != off) {
      at -= off;
      memmove(at, p, len);
    }
    out[i] = at;
    p += len;
  }
  in.remove_prefix(p - in.data());
  return true;
}

/*
 * The n values at the front of in, of cg's type, into cg. Returns false if
 * one is cut short or bad.
 */
bool get_values(std::string_view &in, size_t n, const uint8_t *flags,
                sm2::cg_t *cg) {
  // Each has a length of 4 bytes: no count asks for more memory than its
  // body may fill.
  if (n > in.size() / 4) {
    return false;
  }
  cg->nvalues = static_cast<uint32_t>(n);
  if (cg->type->len < 0) {
    return get_varlenas(in, n, flags, cg->ptrs);
  }
  cg->fixed.reset(new char[n * cg->type->len]);
  return get_fixed(in, n, flags, cg->type->len, cg->fixed.get());
}

/*
 * The integers of type T that a delta body holds: the first, then each next
 * one's difference from the one before. The sums wrap, as the writer's do.
 */
template <typename T>
bool get_deltas(std::string_view &in, size_t n, uint64_t base, uint8_t bits,
                char *out) {
  std::vector<uint64_t> deltas(n ? n - 1 : 0);
  if (!unpack(in, deltas.size(), bits, deltas.data())) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    base += i > 0 ? deltas[i - 1] : 0;
    const T v = static_cast<T>(base);
    memcpy(out + i * sizeof(T), &v, sizeof(T));
  }
  return true;
}

} // namespace

/* Append st's bytes; see sm2.h. */
void sm2::cg_stats_encode(const cg_stats_t &st, std::string &out) {
  sm0::put_be32(out, st.rows);
  sm0::put_be32(out, st.nulls);
  sm0::put_be32(out, st.distinct);
  uint8_t f = (st.min_cut ? st_min_cut : 0) | (st.max_cut ? st_max_cut : 0) |
              (st.has_nan ? st_nan : 0) | (st.has_pos_inf ? st_pos_inf : 0) |
              (st.has_neg_inf ? st_neg_inf : 0) | (st.sorted ? st_sorted : 0);
  out.push_back(static_cast<char>(f));
  put_str(out, st.min);
  put_str(out, st.max);
  put_str(out, st.collation_version);
}

/* Read cg_stats_encode's bytes; see sm2.h. */
int sm2::cg_stats_decode(std::string_view &in, cg_stats_t &st) {
  if (!get_u32(in, st.rows) || !get_u32(in, st.nulls) ||
      !get_u32(in, st.distinct) || in.empty()) {
    return corrupt("statistics cut short");
  }
  uint8_t f = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  st.min_cut = f & st_min_cut;
  st.max_cut = f & st_max_cut;
  st.has_nan = f & st_nan;
  st.has_pos_inf = f & st_pos_inf;
  st.has_neg_inf = f & st_neg_inf;
  st.sorted = f & st_sorted;
  if (!get_str(in, st.min) || !get_str(in, st.max) ||
      !get_str(in, st.collation_version)) {
    return corrupt("statistics cut short");
  }
  return 0;
}

/* Build a CG; see sm2.h. */
int sm2::cg_build(const cg_column_t &col, std::span<const cg_value_t> values,
                  cg_built_t &out) {
  out = cg_built_t();
  cg_stats_t &st = out.stats;
  const size_t n = values.size();
  const specials_t sp = specials_of(col);
  const bool varlena = col.type->len < 0;
  st.rows = static_cast<uint32_t>(n);
  if (col.collation) {
    st.collation_version = std::string(collation_version(col.collation));
  }
  // 1. Each value's key, for the statistics, the filter and order.
  std::vector<std::string> keys(n);
  std::vector<bool> nulls(n, false);
  std::unordered_set<std::string> distinct;
  std::string last;
  bool any = false, finite = false;
  st.sorted = true;
  for (size_t i = 0; i < n; i++) {
    if (values[i].isnull) {
      nulls[i] = true;
      st.nulls++;
      continue;
    }
    std::string_view d = values[i].datum;
    // The key is of the value: a varlena's payload, past its header,
    // as every key of sm2's is (encode_key).
    if (col.type->key_encode(varlena ? sm2::varlena_payload(d) : d, col.typmod,
                             col.collation, keys[i]) != 0) {
      return -1;
    }
    const std::string &k = keys[i];
    distinct.insert(k);
    if (any && k < last) {
      st.sorted = false;
    }
    last = k;
    any = true;
    // NaN and the infinities go to the flags; the rest to min, max.
    if (!sp.nan.empty() && k == sp.nan) {
      st.has_nan = true;
    } else if (!sp.pos_inf.empty() && k == sp.pos_inf) {
      st.has_pos_inf = true;
    } else if (!sp.neg_inf.empty() && k == sp.neg_inf) {
      st.has_neg_inf = true;
    } else {
      if (!finite) {
        st.min = st.max = k;
        finite = true;
      } else {
        if (k < st.min) {
          st.min = k;
        }
        if (k > st.max) {
          st.max = k;
        }
      }
    }
  }
  st.distinct = static_cast<uint32_t>(distinct.size());
  // Cut min and max: a prefix of the min is still a lower bound.
  if (st.min.size() > cg_minmax_max) {
    st.min.resize(cg_minmax_max);
    st.min_cut = true;
  }
  if (st.max.size() > cg_minmax_max) {
    st.max.resize(cg_minmax_max);
    st.max_cut = true;
  }
  // 2. The filter: the exact set, or a bloom filter.
  {
    std::vector<std::string> fk(distinct.begin(), distinct.end());
    std::sort(fk.begin(), fk.end());
    size_t total = 0;
    for (const std::string &k : fk) {
      total += k.size();
    }
    if (fk.empty()) {
      out.filter.push_back(static_cast<char>(filter_none));
    } else if (total <= exact_max) {
      out.filter.push_back(static_cast<char>(filter_exact));
      sm0::put_be32(out.filter, static_cast<uint32_t>(fk.size()));
      for (const std::string &k : fk) {
        put_str(out.filter, k);
      }
    } else {
      uint32_t nbits = static_cast<uint32_t>(fk.size()) * bloom_bits_per_key;
      std::string bits((nbits + 7) / 8, '\0');
      for (const std::string &k : fk) {
        uint64_t h = key_hash(k);
        for (uint8_t p = 0; p < bloom_probes; p++) {
          uint32_t b = probe_bit(h, p, nbits);
          bits[b / 8] = static_cast<char>(bits[b / 8] | (1 << (b % 8)));
        }
      }
      out.filter.push_back(static_cast<char>(filter_bloom));
      out.filter.push_back(static_cast<char>(bloom_probes));
      sm0::put_be32(out.filter, nbits);
      out.filter.append(bits);
    }
  }
  // 3. Each row's datum; every value is in the CG.
  std::vector<std::string> datums(n);
  for (size_t i = 0; i < n; i++) {
    if (!nulls[i]) {
      datums[i] = std::string(values[i].datum);
    }
  }
  // 4. The encoding: delta for sorted integers with no null, else
  // a dictionary where it is smaller than plain, else plain.
  uint8_t enc = enc_plain;
  std::vector<std::string> dict;
  if (is_int(col.type) && st.nulls == 0 && st.sorted && n > 0) {
    enc = enc_delta;
  } else if (n > 0) {
    // The dictionary holds datums, in key order; equal keys under a
    // non-deterministic collation are distinct datums still.
    std::vector<std::pair<std::string, std::string>> kd;
    {
      std::unordered_set<std::string> seen;
      for (size_t i = 0; i < n; i++) {
        if (!nulls[i] && seen.insert(datums[i]).second) {
          kd.emplace_back(keys[i], datums[i]);
        }
      }
    }
    std::sort(kd.begin(), kd.end());
    size_t plain = 0, dsize = 4 + 4 * (kd.size() + 1);
    for (size_t i = 0; i < n; i++) {
      plain += varlena ? datums[i].size() + 4 : datums[i].size();
    }
    for (const auto &[k, d] : kd) {
      dsize += d.size();
    }
    dsize += (n * bits_for(kd.empty() ? 0 : kd.size() - 1) + 7) / 8 + 1;
    if (!kd.empty() && dsize < plain) {
      enc = enc_dict;
      for (auto &[k, d] : kd) {
        dict.push_back(std::move(d));
      }
    }
  }
  // 5. The body.
  std::string body;
  body.push_back(static_cast<char>(enc));
  sm0::put_be32(body, static_cast<uint32_t>(n));
  uint8_t has_flags = st.nulls ? 1 : 0;
  body.push_back(static_cast<char>(has_flags));
  for (size_t i = 0; st.nulls && i < n; i++) {
    body.push_back(nulls[i] ? static_cast<char>(flag_null) : '\0');
  }
  if (enc == enc_delta) {
    int64_t base = int_of(datums[0]);
    std::vector<uint64_t> deltas;
    uint64_t maxd = 0;
    for (size_t i = 1; i < n; i++) {
      // Unsigned: the difference of two int64 may pass int64.
      uint64_t dl = static_cast<uint64_t>(int_of(datums[i])) -
                    static_cast<uint64_t>(int_of(datums[i - 1]));
      deltas.push_back(dl);
      maxd = std::max(maxd, dl);
    }
    sm0::put_be64(body, static_cast<uint64_t>(base));
    body.push_back(static_cast<char>(datums[0].size()));
    uint8_t bits = bits_for(maxd);
    body.push_back(static_cast<char>(bits));
    pack(body, deltas, bits);
  } else if (enc == enc_dict) {
    sm0::put_be32(body, static_cast<uint32_t>(dict.size()));
    for (const std::string &d : dict) {
      put_str(body, d);
    }
    std::vector<uint64_t> codes(n, 0);
    // Each row's code: its datum's place in the dictionary.
    std::vector<std::pair<std::string_view, uint32_t>> at;
    for (uint32_t c = 0; c < dict.size(); c++) {
      at.emplace_back(dict[c], c);
    }
    std::sort(at.begin(), at.end());
    for (size_t i = 0; i < n; i++) {
      if (nulls[i]) {
        continue;
      }
      auto it = std::lower_bound(
          at.begin(), at.end(), std::string_view(datums[i]),
          [](const auto &a, std::string_view b) { return a.first < b; });
      codes[i] = it->second;
    }
    uint8_t bits = bits_for(dict.size() - 1);
    body.push_back(static_cast<char>(bits));
    pack(body, codes, bits);
  } else {
    for (size_t i = 0; i < n; i++) {
      put_str(body, datums[i]);
    }
  }
  // 6. Compress the body, and lay out the file.
  std::string z(ZSTD_compressBound(body.size()), '\0');
  size_t zn = ZSTD_compress(z.data(), z.size(), body.data(), body.size(), 3);
  if (ZSTD_isError(zn)) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       std::string("CG: zstd: ") + ZSTD_getErrorName(zn));
  }
  z.resize(zn);
  std::string stats;
  cg_stats_encode(st, stats);
  std::string &f = out.file;
  f.append(z);
  f.append(stats);
  f.append(out.filter);
  sm0::put_be32(f, cg_magic);
  sm0::put_be32(f, cg_format);
  f.push_back(static_cast<char>(enc));
  f.push_back(0);
  sm0::put_be32(f, static_cast<uint32_t>(n));
  sm0::put_be32(f, static_cast<uint32_t>(z.size()));
  sm0::put_be32(f, static_cast<uint32_t>(body.size()));
  sm0::put_be32(f, static_cast<uint32_t>(stats.size()));
  sm0::put_be32(f, static_cast<uint32_t>(out.filter.size()));
  sm0::put_be32(f, sm0::crc32c(0, f.data(), f.size()));
  sm0::put_be32(f, cg_magic);
  return 0;
}

/*
 * Check and decode a CG file; see sm2.h.
 * 1. Check the tail, the CRC32C and the sizes, and decompress the body.
 * 2. The flag bytes, if a row is null.
 * 3. The values, by the encoding. A fixed type's are copied to an array; a
 *    varlena type's stay in the body.
 * 4. Keep the body if the values are in it.
 */
int sm2::cg_open(std::string_view file, const type_t *type, cg_t *&out) {
  out = nullptr;
  // 1. The tail, and the body.
  if (file.size() < tail_size) {
    return corrupt("file shorter than its tail");
  }
  const char *t = file.data() + file.size() - tail_size;
  if (sm0::get_be32(t) != cg_magic ||
      sm0::get_be32(t + tail_size - 4) != cg_magic) {
    return corrupt("bad magic");
  }
  if (sm0::get_be32(t + 4) != cg_format) {
    return corrupt("unknown format");
  }
  uint32_t rows = sm0::get_be32(t + 10);
  uint32_t zlen = sm0::get_be32(t + 14);
  uint32_t rawlen = sm0::get_be32(t + 18);
  uint32_t slen = sm0::get_be32(t + 22);
  uint32_t flen = sm0::get_be32(t + 26);
  uint32_t crc = sm0::get_be32(t + 30);
  size_t covered = file.size() - 8;
  if (sm0::crc32c(0, file.data(), covered) != crc) {
    return corrupt("CRC32C mismatch");
  }
  if (uint64_t(zlen) + slen + flen + tail_size != file.size()) {
    return corrupt("part sizes do not add up");
  }
  std::string body(rawlen + body_spare, '\0');
  size_t got = ZSTD_decompress(body.data(), rawlen, file.data(), zlen);
  if (ZSTD_isError(got) || got != rawlen) {
    return corrupt("bad zstd body");
  }
  std::string_view in(body.data(), rawlen);
  if (in.size() < 6) {
    return corrupt("body cut short");
  }
  uint8_t enc = static_cast<uint8_t>(in[0]);
  uint32_t n = sm0::get_be32(in.data() + 1);
  uint8_t has_flags = static_cast<uint8_t>(in[5]);
  in.remove_prefix(6);
  if (n != rows) {
    return corrupt("row counts differ");
  }
  std::unique_ptr<cg_t> cg(new cg_t);
  cg->type = type;
  cg->n = n;
  // 2. The flag bytes.
  if (has_flags && !get_flags(in, n, cg->flags)) {
    return corrupt("bad flag bytes");
  }
  const uint8_t *flags = has_flags ? cg->flags.data() : nullptr;
  // 3. The values.
  if (enc == enc_delta) {
    if (in.size() < 10) {
      return corrupt("delta header cut short");
    }
    uint64_t base = sm0::get_be64(in.data());
    uint8_t width = static_cast<uint8_t>(in[8]);
    uint8_t bits = static_cast<uint8_t>(in[9]);
    in.remove_prefix(10);
    if (!is_int(type) || width != type->len || flags) {
      return corrupt("delta of a column that has none");
    }
    cg->nvalues = n;
    cg->fixed.reset(new char[size_t(n) * width]);
    char *p = cg->fixed.get();
    if (!(width == 2   ? get_deltas<int16_t>(in, n, base, bits, p)
          : width == 4 ? get_deltas<int32_t>(in, n, base, bits, p)
                       : get_deltas<int64_t>(in, n, base, bits, p))) {
      return corrupt("deltas cut short");
    }
  } else if (enc == enc_dict) {
    uint32_t dn;
    if (!get_u32(in, dn)) {
      return corrupt("dictionary cut short");
    }
    if (dn > n) {
      return corrupt("dictionary larger than its CG");
    }
    if (!get_values(in, dn, nullptr, cg.get()) || in.empty()) {
      return corrupt("bad dictionary");
    }
    uint8_t bits = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    cg->dict = true;
    cg->codes.resize(n);
    if (!unpack(in, n, bits, cg->codes.data())) {
      return corrupt("codes cut short");
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
      bad |= cg->codes[i] >= dn;
    }
    if (bad) {
      return corrupt("code past the dictionary");
    }
  } else if (enc == enc_plain) {
    if (!get_values(in, n, flags, cg.get())) {
      return corrupt("bad values");
    }
  } else {
    return corrupt("unknown encoding");
  }
  // 4. The body.
  if (type->len < 0) {
    cg->body = std::move(body);
  }
  out = cg.release();
  return 0;
}

/* Free a CG; see sm2.h. */
void sm2::cg_close(cg_t *cg) { delete cg; }

/* The rows of a CG; see sm2.h. */
uint32_t sm2::cg_rows(const cg_t *cg) { return cg->n; }

/* A CG's arrays; see sm2.h. */
void sm2::cg_vec(const cg_t *cg, cg_vec_t &out) {
  out.n = cg->n;
  out.values = cg->type->len < 0 ? static_cast<const void *>(cg->ptrs.data())
                                 : cg->fixed.get();
  out.codes = cg->dict ? cg->codes.data() : nullptr;
  out.dict_n = cg->dict ? cg->nvalues : 0;
  out.flags = cg->flags.empty() ? nullptr : cg->flags.data();
}

/* Row i of a CG; see sm2.h. */
int sm2::cg_get(const cg_t *cg, uint32_t i, std::string_view &datum) {
  datum = std::string_view();
  if (!cg->flags.empty() && cg->flags[i]) {
    return 1;
  }
  const uint32_t k = cg->dict ? cg->codes[i] : i;
  if (cg->type->len < 0) {
    datum = varlena_at(cg->ptrs[k]);
  } else {
    const size_t len = cg->type->len;
    datum = {cg->fixed.get() + k * len, len};
  }
  return 0;
}

/* Whether a CG may hold a row meeting b; see sm2.h. */
bool sm2::cg_may_match(const cg_column_t &col, const cg_stats_t &st,
                       std::string_view filter, const cg_bound_t &b) {
  // IS NULL needs a null; any other test a value.
  if (b.is_null && st.nulls == 0) {
    return false;
  }
  bool needs_value = b.not_null || b.has_lo || b.has_hi || b.has_in;
  if (needs_value && st.nulls == st.rows) {
    return false;
  }
  if (!b.has_lo && !b.has_hi && !b.has_in) {
    return true;
  }
  // Keys of another version of the collation may sort otherwise:
  // min, max and the filter say nothing (../../COLSTORE.md §6.2).
  if (col.collation &&
      st.collation_version != collation_version(col.collation)) {
    return true;
  }
  const specials_t sp = specials_of(col);
  if (b.has_lo && !may_be_above(st, sp, b.lo, b.lo_incl)) {
    return false;
  }
  if (b.has_hi && !may_be_below(st, sp, b.hi, b.hi_incl)) {
    return false;
  }
  // An equality: the filter too.
  if (b.has_lo && b.has_hi && b.lo_incl && b.hi_incl && b.lo == b.hi &&
      !filter_may_hold(col, st, filter, b.lo)) {
    return false;
  }
  if (b.has_in) {
    for (const std::string &k : b.in) {
      if (may_be_above(st, sp, k, true) && may_be_below(st, sp, k, true) &&
          filter_may_hold(col, st, filter, k)) {
        return true;
      }
    }
    return false;
  }
  return true;
}
