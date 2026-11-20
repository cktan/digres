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
 * The body, before zstd: the encoding byte, the row count, a null
 * bitmap and a long bitmap (each present only if a row needs it),
 * then the values as the encoding lays them out.
 */

#include "sm2.h"

#include "collation.h"
#include "sm0_codec.h"
#include "sm0_crc.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
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
constexpr uint32_t cg_format = 1;
/* magic, format, encoding, flags, rows, 4 sizes, CRC32C, magic. */
constexpr size_t tail_size = 4 + 4 + 1 + 1 + 4 + 4 * 4 + 4 + 4;

/* The stats' flag bits. */
enum : uint8_t {
  st_min_cut = 0x01,
  st_max_cut = 0x02,
  st_nan = 0x04,
  st_pos_inf = 0x08,
  st_neg_inf = 0x10,
  st_long = 0x20,
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

/* Read n values of width bits each from in; false if cut short. */
bool unpack(std::string_view &in, size_t n, uint8_t bits,
            std::vector<uint64_t> &v) {
  size_t bytes = (n * bits + 7) / 8;
  if (in.size() < bytes || bits > 64) {
    return false;
  }
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
  v.assign(n, 0);
  size_t bit = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t x = 0;
    for (uint8_t b = 0; b < bits; b++, bit++) {
      if ((p[bit / 8] >> (bit % 8)) & 1) {
        x |= uint64_t(1) << b;
      }
    }
    v[i] = x;
  }
  in.remove_prefix(bytes);
  return true;
}

/* Append a bitmap of n flags. */
void put_bitmap(std::string &out, const std::vector<bool> &f) {
  size_t start = out.size();
  out.append((f.size() + 7) / 8, '\0');
  for (size_t i = 0; i < f.size(); i++) {
    if (f[i]) {
      out[start + i / 8] =
          static_cast<char>(out[start + i / 8] | (1 << (i % 8)));
    }
  }
}

/* Read a bitmap of n flags from in; false if cut short. */
bool get_bitmap(std::string_view &in, size_t n, std::vector<bool> &f) {
  size_t bytes = (n + 7) / 8;
  if (in.size() < bytes) {
    return false;
  }
  f.assign(n, false);
  for (size_t i = 0; i < n; i++) {
    f[i] = (in[i / 8] >> (i % 8)) & 1;
  }
  in.remove_prefix(bytes);
  return true;
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

/* The special keys of col's type: by its text, as PostgreSQL's. */
specials_t specials_of(const sm2::cg_column_t &col) {
  specials_t s;
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

/* The datum of width bytes for the integer v. */
std::string int_datum(int64_t v, size_t width) {
  std::string d(width, '\0');
  if (width == 2) {
    int16_t x = static_cast<int16_t>(v);
    memcpy(d.data(), &x, 2);
  } else if (width == 4) {
    int32_t x = static_cast<int32_t>(v);
    memcpy(d.data(), &x, 4);
  } else {
    memcpy(d.data(), &v, 8);
  }
  return d;
}

/* A long value's pointer, as a plain varlena entry holds it. */
std::string long_entry(const sm2::cg_long_t &w) {
  std::string e;
  sm0::put_be64(e, w.offset);
  sm0::put_be32(e, w.length);
  sm0::put_be32(e, w.crc);
  return e;
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
  // A long value is in no filter, and its key's size says nothing
  // of its own: a bpchar's key leaves out its trailing spaces, and a
  // non-deterministic collation calls strings of other lengths equal.
  if (st.has_long) {
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

/* A CG decoded: each row's datum, or null, or a long pointer. */
struct sm2::cg_t {
  std::vector<std::string> datums;
  std::vector<bool> nulls;
  std::vector<bool> longs;
};

/* Append st's bytes; see sm2.h. */
void sm2::cg_stats_encode(const cg_stats_t &st, std::string &out) {
  sm0::put_be32(out, st.rows);
  sm0::put_be32(out, st.nulls);
  sm0::put_be32(out, st.distinct);
  uint8_t f = (st.min_cut ? st_min_cut : 0) | (st.max_cut ? st_max_cut : 0) |
              (st.has_nan ? st_nan : 0) | (st.has_pos_inf ? st_pos_inf : 0) |
              (st.has_neg_inf ? st_neg_inf : 0) | (st.has_long ? st_long : 0) |
              (st.sorted ? st_sorted : 0);
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
  st.has_long = f & st_long;
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
  std::vector<bool> nulls(n, false), longs(n, false);
  std::unordered_set<std::string> distinct, filtered;
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
    if (col.type->key_encode(d, col.typmod, col.collation, keys[i]) != 0) {
      return -1;
    }
    const std::string &k = keys[i];
    longs[i] = varlena && d.size() > cg_long_min;
    st.has_long = st.has_long || longs[i];
    distinct.insert(k);
    // A long value is in no filter (§6.3).
    if (!longs[i]) {
      filtered.insert(k);
    }
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
    std::vector<std::string> fk(filtered.begin(), filtered.end());
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
  // 3. Long values to their file; a pointer stands in each one's row.
  std::vector<std::string> datums(n);
  for (size_t i = 0; i < n; i++) {
    if (nulls[i]) {
      continue;
    }
    std::string_view d = values[i].datum;
    if (longs[i]) {
      cg_long_t w;
      w.offset = out.long_file.size();
      w.length = static_cast<uint32_t>(d.size());
      w.crc = sm0::crc32c(d);
      out.long_file.append(d.data(), d.size());
      datums[i] = long_entry(w);
    } else {
      datums[i] = std::string(d);
    }
  }
  // 4. The encoding: delta for sorted integers with no null, else
  // a dictionary where it is smaller than plain, else plain.
  uint8_t enc = enc_plain;
  std::vector<std::string> dict;
  if (is_int(col.type) && st.nulls == 0 && st.sorted && n > 0) {
    enc = enc_delta;
  } else if (!st.has_long && n > 0) {
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
  uint8_t maps = (st.nulls ? 1 : 0) | (st.has_long ? 2 : 0);
  body.push_back(static_cast<char>(maps));
  if (st.nulls) {
    put_bitmap(body, nulls);
  }
  if (st.has_long) {
    put_bitmap(body, longs);
  }
  if (enc == enc_delta) {
    int64_t base = int_of(datums[0]);
    std::vector<uint64_t> deltas;
    uint64_t maxd = 0;
    for (size_t i = 1; i < n; i++) {
      uint64_t dl =
          static_cast<uint64_t>(int_of(datums[i]) - int_of(datums[i - 1]));
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

/* Check and decode a CG file; see sm2.h. */
int sm2::cg_open(std::string_view file, cg_t *&out) {
  out = nullptr;
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
  std::string body(rawlen, '\0');
  size_t got = ZSTD_decompress(body.data(), body.size(), file.data(), zlen);
  if (ZSTD_isError(got) || got != rawlen) {
    return corrupt("bad zstd body");
  }
  std::string_view in = body;
  if (in.size() < 6) {
    return corrupt("body cut short");
  }
  uint8_t enc = static_cast<uint8_t>(in[0]);
  uint32_t n = sm0::get_be32(in.data() + 1);
  uint8_t maps = static_cast<uint8_t>(in[5]);
  in.remove_prefix(6);
  if (n != rows) {
    return corrupt("row counts differ");
  }
  std::unique_ptr<cg_t> cg(new cg_t);
  cg->datums.resize(n);
  cg->nulls.assign(n, false);
  cg->longs.assign(n, false);
  if ((maps & 1) && !get_bitmap(in, n, cg->nulls)) {
    return corrupt("null bitmap cut short");
  }
  if ((maps & 2) && !get_bitmap(in, n, cg->longs)) {
    return corrupt("long bitmap cut short");
  }
  if (enc == enc_delta) {
    if (in.size() < 10) {
      return corrupt("delta header cut short");
    }
    int64_t v = static_cast<int64_t>(sm0::get_be64(in.data()));
    size_t width = static_cast<uint8_t>(in[8]);
    uint8_t bits = static_cast<uint8_t>(in[9]);
    in.remove_prefix(10);
    std::vector<uint64_t> deltas;
    if (n > 0 && !unpack(in, n - 1, bits, deltas)) {
      return corrupt("deltas cut short");
    }
    for (uint32_t i = 0; i < n; i++) {
      if (i > 0) {
        v += static_cast<int64_t>(deltas[i - 1]);
      }
      cg->datums[i] = int_datum(v, width);
    }
  } else if (enc == enc_dict) {
    uint32_t dn;
    if (!get_u32(in, dn)) {
      return corrupt("dictionary cut short");
    }
    std::vector<std::string> dict(dn);
    for (uint32_t c = 0; c < dn; c++) {
      if (!get_str(in, dict[c])) {
        return corrupt("dictionary cut short");
      }
    }
    if (in.empty()) {
      return corrupt("codes cut short");
    }
    uint8_t bits = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    std::vector<uint64_t> codes;
    if (!unpack(in, n, bits, codes)) {
      return corrupt("codes cut short");
    }
    for (uint32_t i = 0; i < n; i++) {
      if (cg->nulls[i]) {
        continue;
      }
      if (codes[i] >= dn) {
        return corrupt("code past the dictionary");
      }
      cg->datums[i] = dict[codes[i]];
    }
  } else if (enc == enc_plain) {
    for (uint32_t i = 0; i < n; i++) {
      if (!get_str(in, cg->datums[i])) {
        return corrupt("values cut short");
      }
    }
  } else {
    return corrupt("unknown encoding");
  }
  out = cg.release();
  return 0;
}

/* Free a CG; see sm2.h. */
void sm2::cg_close(cg_t *cg) { delete cg; }

/* The rows of a CG; see sm2.h. */
uint32_t sm2::cg_rows(const cg_t *cg) {
  return static_cast<uint32_t>(cg->datums.size());
}

/* Row i of a CG; see sm2.h. */
int sm2::cg_get(const cg_t *cg, uint32_t i, std::string_view &datum,
                cg_long_t &where) {
  datum = std::string_view();
  if (cg->nulls[i]) {
    return 1;
  }
  const std::string &d = cg->datums[i];
  if (cg->longs[i]) {
    where.offset = sm0::get_be64(d.data());
    where.length = sm0::get_be32(d.data() + 8);
    where.crc = sm0::get_be32(d.data() + 12);
    return 2;
  }
  datum = d;
  return 0;
}

/* Check a long value's bytes; see sm2.h. */
int sm2::cg_long_check(std::string_view bytes, const cg_long_t &where) {
  if (bytes.size() != where.length || sm0::crc32c(bytes) != where.crc) {
    return corrupt("long value CRC32C mismatch");
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
