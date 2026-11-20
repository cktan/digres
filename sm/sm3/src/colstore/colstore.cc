/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * colstore.cc -- a column store table's row groups (../../COLSTORE.md
 * §4, §9.2, §12): their RG keys and filter keys, the files of their
 * CGs through sm1's file sets, built from rows and read back.
 */

#include "colstore.h"

#include "sm0_codec.h"
#include "sm0_crc.h"

#include <cstring>

namespace {

/* The version of an RG key's value. */
constexpr uint32_t rg_meta_version = 1;

/* Fail with SM_ERR_CORRUPT: what is wrong with a row group. */
int corrupt(const std::string &what) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "column store: " + what);
}

/* Append v as two bytes, big-endian, as a relid is. */
void put_be16(std::string &k, uint16_t v) {
  k.push_back(static_cast<char>(v >> 8));
  k.push_back(static_cast<char>(v & 0xFF));
}

/* The key of a filter: relid, rg_id, and the column, _id's 0. */
std::string filter_key(uint64_t rg_id, int32_t attnum) {
  std::string k;
  put_be16(k, sm3::filter_relid);
  sm0::put_be64(k, rg_id);
  put_be16(k, static_cast<uint16_t>(attnum + 1));
  return k;
}

/* The bounds of row group rg_id's delete markers' keys. */
std::string marker_bound(uint64_t rg_id, unsigned char fill) {
  std::string k;
  put_be16(k, sm3::marker_relid);
  sm0::put_be64(k, rg_id);
  k.append(8, static_cast<char>(fill));
  return k;
}

} // namespace

/* The key of a row's delete marker; see colstore.h. */
std::string sm3::marker_key(uint64_t rg_id, uint64_t id) {
  std::string k;
  put_be16(k, sm3::marker_relid);
  sm0::put_be64(k, rg_id);
  sm0::put_be64(k, id);
  return k;
}

namespace {

/* The int8 datum bytes of _id id, as sm2 lays an int8 out. */
std::string id_bytes(uint64_t id) {
  std::string d(8, '\0');
  memcpy(d.data(), &id, 8);
  return d;
}

/* A datum_t of type for its bytes b, which it views if by reference. */
sm2::datum_t to_datum(const sm2::type_t *type, std::string_view b) {
  if (!type->byval) {
    return reinterpret_cast<sm2::datum_t>(b.data());
  }
  // A by-value datum is sign-extended, as sm2.h says.
  switch (b.size()) {
  case 1:
    return static_cast<sm2::datum_t>(static_cast<unsigned char>(b[0]));
  case 2: {
    int16_t v;
    memcpy(&v, b.data(), 2);
    return sm2::datum_from_int16(v);
  }
  case 4: {
    int32_t v;
    memcpy(&v, b.data(), 4);
    return sm2::datum_from_int32(v);
  }
  default: {
    uint64_t v = 0;
    memcpy(&v, b.data(), std::min<size_t>(8, b.size()));
    return static_cast<sm2::datum_t>(v);
  }
  }
}

/* Append a 32-bit length, then the bytes of s. */
void put_str(std::string &out, std::string_view s) {
  sm0::put_be32(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

} // namespace

namespace sm3 {

/* The file name of attnum's CG; see colstore.h. */
std::string rg_file_name(int32_t attnum) {
  return attnum < 0 ? std::string(rg_id_name) : std::to_string(attnum);
}

/* Encode an RG key's value; see colstore.h. */
void rg_meta_encode(const rg_meta_t &meta, std::string &out) {
  out.clear();
  sm0::put_be32(out, rg_meta_version);
  sm0::put_be32(out, meta.rows);
  sm0::put_be64(out, meta.id_min);
  sm0::put_be64(out, meta.id_max);
  sm0::put_be32(out, static_cast<uint32_t>(meta.cols.size()));
  for (const rg_col_t &c : meta.cols) {
    sm0::put_be32(out, static_cast<uint32_t>(c.attnum));
    sm0::put_be64(out, c.size);
    sm0::put_be32(out, c.crc);
    sm0::put_be64(out, c.long_size);
    sm0::put_be32(out, c.long_crc);
    std::string st;
    sm2::cg_stats_encode(c.stats, st);
    put_str(out, st);
  }
}

/* Decode an RG key's value; see colstore.h. */
int rg_meta_decode(std::string_view in, rg_meta_t &meta) {
  meta = rg_meta_t();
  if (in.size() < 28 || sm0::get_be32(in.data()) != rg_meta_version) {
    return corrupt("an RG key's value is cut short, or of another version");
  }
  meta.rows = sm0::get_be32(in.data() + 4);
  meta.id_min = sm0::get_be64(in.data() + 8);
  meta.id_max = sm0::get_be64(in.data() + 16);
  uint32_t n = sm0::get_be32(in.data() + 24);
  in.remove_prefix(28);
  for (uint32_t i = 0; i < n; i++) {
    if (in.size() < 32) {
      return corrupt("an RG key's column is cut short");
    }
    rg_col_t c;
    c.attnum = static_cast<int32_t>(sm0::get_be32(in.data()));
    c.size = sm0::get_be64(in.data() + 4);
    c.crc = sm0::get_be32(in.data() + 12);
    c.long_size = sm0::get_be64(in.data() + 16);
    c.long_crc = sm0::get_be32(in.data() + 24);
    uint32_t len = sm0::get_be32(in.data() + 28);
    in.remove_prefix(32);
    if (in.size() < len) {
      return corrupt("an RG key's statistics are cut short");
    }
    std::string_view st = in.substr(0, len);
    if (sm2::cg_stats_decode(st, c.stats) != 0) {
      return -1;
    }
    in.remove_prefix(len);
    meta.cols.push_back(std::move(c));
  }
  return 0;
}

/* Begin building row groups; see colstore.h. */
void rg_build_begin(rg_build_t &b, table_t *table) {
  b = rg_build_t();
  b.table = table;
  size_t n = tbl_def(table).columns.size();
  b.vals.resize(n);
  b.nulls.resize(n);
}

/* Add a row; see colstore.h. */
int rg_build_add(rg_build_t &b, uint64_t id,
                 std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull) {
  const table_def_t &def = tbl_def(b.table);
  for (size_t i = 0; i < def.columns.size(); i++) {
    const column_def_t &c = def.columns[i];
    bool null = c.dropped || i >= isnull.size() || isnull[i];
    b.nulls[i].push_back(null);
    if (null) {
      b.vals[i].emplace_back();
      continue;
    }
    std::string_view bytes;
    if (sm2::datum_bytes(c.type, c.typmod, values[i], bytes) != 0) {
      return -1;
    }
    // A value out of line is read back: a CG holds no TOAST pointer
    // (../COLSTORE.md §5).
    if (c.type->len < 0 && sm2::varlena_is_toast(bytes)) {
      std::string full;
      if (tbl_fetch(b.table, bytes, full) != 0) {
        return -1;
      }
      b.vals[i].push_back(std::move(full));
      continue;
    }
    b.vals[i].emplace_back(bytes);
  }
  b.ids.push_back(id);
  return 0;
}

/* Write one row group; see colstore.h. */
int rg_build_flush(rg_build_t &b) {
  const size_t n = b.ids.size();
  if (n == 0) {
    return 0;
  }
  table_t *t = b.table;
  const table_def_t &def = tbl_def(t);
  sm1::session_t *s = tbl_session(t)->sm1;
  // 1. Draw the row group's id: never used again (../COLSTORE.md §4.1).
  uint64_t rg_id = 0;
  if (sm1::nextseq(s, rg_seq_name(def.oid), rg_id) != 0) {
    return -1;
  }
  rg_meta_t meta;
  meta.rows = static_cast<uint32_t>(n);
  meta.id_min = UINT64_MAX;
  // 2. The CG of each column, _id's first, and its filter key.
  auto write_cg = [&](int32_t attnum, const sm2::cg_column_t &col,
                      const std::vector<sm2::cg_value_t> &v) -> int {
    sm2::cg_built_t built;
    if (sm2::cg_build(col, v, built) != 0) {
      return -1;
    }
    std::string name = rg_file_name(attnum);
    if (sm1::fileset_write(s, def.storeid, rg_id, name, built.file) != 0) {
      return -1;
    }
    rg_col_t c;
    c.attnum = attnum;
    c.size = built.file.size();
    c.crc = sm0::crc32c(built.file);
    if (!built.long_file.empty()) {
      if (sm1::fileset_write(s, def.storeid, rg_id, name + ".long",
                             built.long_file) != 0) {
        return -1;
      }
      c.long_size = built.long_file.size();
      c.long_crc = sm0::crc32c(built.long_file);
    }
    c.stats = std::move(built.stats);
    if (sm1::insert_new(s, def.storeid, filter_key(rg_id, attnum),
                        built.filter) != 0) {
      return -1;
    }
    meta.cols.push_back(std::move(c));
    return 0;
  };
  {
    std::vector<std::string> ib(n);
    std::vector<sm2::cg_value_t> v(n);
    for (size_t r = 0; r < n; r++) {
      meta.id_min = std::min(meta.id_min, b.ids[r]);
      meta.id_max = std::max(meta.id_max, b.ids[r]);
      ib[r] = id_bytes(b.ids[r]);
      v[r] = {ib[r], false};
    }
    if (write_cg(-1, {&sm2::type_int8, -1, nullptr}, v) != 0) {
      return -1;
    }
  }
  for (size_t i = 0; i < def.columns.size(); i++) {
    const column_def_t &c = def.columns[i];
    if (c.dropped) {
      continue;
    }
    std::vector<sm2::cg_value_t> v(n);
    for (size_t r = 0; r < n; r++) {
      v[r] = {b.vals[i][r], static_cast<bool>(b.nulls[i][r])};
    }
    if (write_cg(static_cast<int32_t>(i), {c.type, c.typmod, c.collation}, v) !=
        0) {
      return -1;
    }
  }
  // 3. The RG key, once its files are durable (../COLSTORE.md §13).
  std::string payload;
  rg_meta_encode(meta, payload);
  if (sm1::fileset_put(s, def.storeid, rg_id, payload) != 0) {
    return -1;
  }
  // 4. The next row group starts empty.
  b.ids.clear();
  for (size_t i = 0; i < b.vals.size(); i++) {
    b.vals[i].clear();
    b.nulls[i].clear();
  }
  return 0;
}

/* Free a row group read; see colstore.h. */
void rg_read_close(rg_read_t &r) {
  for (sm2::cg_t *cg : r.cgs) {
    sm2::cg_close(cg);
  }
  sm2::cg_close(r.ids);
  r = rg_read_t();
}

/* Read a row group for a scan; see colstore.h. */
int rg_read_open(table_t *table, uint64_t rg_id, rg_meta_t meta,
                 const std::vector<bool> &cols, rg_read_t &r) {
  rg_read_close(r);
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  r.rg_id = rg_id;
  r.meta = std::move(meta);
  r.cgs.assign(def.columns.size(), nullptr);
  r.longs.assign(def.columns.size(), std::string());
  // 1. Each CG wanted: its file, checked against its RG key.
  for (const rg_col_t &c : r.meta.cols) {
    bool want =
        c.attnum < 0 ||
        (static_cast<size_t>(c.attnum) < def.columns.size() &&
         !def.columns[c.attnum].dropped &&
         (cols.empty() ||
          (static_cast<size_t>(c.attnum) < cols.size() && cols[c.attnum])));
    if (!want) {
      continue;
    }
    std::string file;
    if (sm1::fileset_read(s, def.storeid, rg_id, rg_file_name(c.attnum), 0,
                          UINT64_MAX, file) != 0) {
      return -1;
    }
    if (file.size() != c.size || sm0::crc32c(file) != c.crc) {
      return corrupt("row group " + std::to_string(rg_id) + "'s CG " +
                     rg_file_name(c.attnum) + " is not what its key names");
    }
    sm2::cg_t *cg = nullptr;
    if (sm2::cg_open(file, cg) != 0) {
      return -1;
    }
    if (c.attnum < 0) {
      r.ids = cg;
    } else {
      r.cgs[c.attnum] = cg;
    }
  }
  if (!r.ids) {
    return corrupt("row group " + std::to_string(rg_id) + " has no _id CG");
  }
  // 2. Its delete markers, at the transaction's view.
  return col_markers(table, rg_id, r.deleted);
}

/* A row group's delete markers; see colstore.h. */
int col_markers(table_t *table, uint64_t rg_id, std::set<uint64_t> &ids) {
  std::string lo = marker_bound(rg_id, 0x00), hi = marker_bound(rg_id, 0xFF);
  sm1::scan_t *c = nullptr;
  if (sm1::scan(tbl_session(table)->sm1, tbl_def(table).storeid, lo.data(),
                lo.size(), 1, hi.data(), hi.size(), 1, c) != 0) {
    return -1;
  }
  std::string_view k, d;
  int rc;
  while ((rc = sm1::scannxt(c, k, d)) == 0) {
    if (k.size() == 18) {
      ids.insert(sm0::get_be64(k.data() + 10));
    }
  }
  sm1::scanend(c);
  return rc < 0 ? -1 : 0;
}

/* The bytes of the row groups' CGs; see colstore.h. */
int col_bytes(table_t *table, uint64_t &bytes) {
  bytes = 0;
  std::vector<std::pair<uint64_t, std::string>> rgs;
  if (sm1::fileset_list(tbl_session(table)->sm1, tbl_def(table).storeid, rgs) !=
      0) {
    return -1;
  }
  for (const auto &[rg_id, value] : rgs) {
    rg_meta_t meta;
    if (rg_meta_decode(value, meta) != 0) {
      return -1;
    }
    for (const rg_col_t &c : meta.cols) {
      bytes += c.size + c.long_size;
    }
  }
  return 0;
}

/* The next live row of a row group; see colstore.h. */
int rg_read_next(table_t *table, rg_read_t &r, uint64_t &id,
                 std::span<sm2::datum_t> values, std::span<bool> isnull) {
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  for (size_t c = 0; c < values.size(); c++) {
    values[c] = 0;
    isnull[c] = true;
  }
  while (r.at < sm2::cg_rows(r.ids)) {
    uint32_t i = r.at++;
    std::string_view d;
    sm2::cg_long_t w;
    if (sm2::cg_get(r.ids, i, d, w) != 0 || d.size() != 8) {
      return corrupt("an _id CG's row is not an _id");
    }
    uint64_t row_id;
    memcpy(&row_id, d.data(), 8);
    // A row its delete marker names is gone from this row group.
    if (r.deleted.count(row_id)) {
      continue;
    }
    for (size_t c = 0; c < def.columns.size(); c++) {
      if (!r.cgs[c]) {
        continue;
      }
      int g = sm2::cg_get(r.cgs[c], i, d, w);
      if (g == 1) {
        continue;
      }
      if (g == 2) {
        // A long value: one ranged read of its file.
        if (sm1::fileset_read(s, def.storeid, r.rg_id,
                              rg_file_name(static_cast<int32_t>(c)) + ".long",
                              w.offset, w.length, r.longs[c]) != 0 ||
            sm2::cg_long_check(r.longs[c], w) != 0) {
          return -1;
        }
        d = r.longs[c];
      }
      values[c] = to_datum(def.columns[c].type, d);
      isnull[c] = false;
    }
    id = row_id;
    return 0;
  }
  return 1;
}

/* Put a bulk load's last rows into the delta store; see colstore.h. */
int rg_build_to_delta(
    rg_build_t &b,
    const std::function<int(uint64_t, std::span<const sm2::datum_t>,
                            std::span<const bool>)> &insert) {
  const table_def_t &def = tbl_def(b.table);
  const size_t ncols = def.columns.size();
  std::vector<sm2::datum_t> values(ncols);
  std::unique_ptr<bool[]> isnull(new bool[ncols]);
  for (size_t r = 0; r < b.ids.size(); r++) {
    for (size_t c = 0; c < ncols; c++) {
      isnull[c] = b.nulls[c][r];
      values[c] = isnull[c] ? 0 : to_datum(def.columns[c].type, b.vals[c][r]);
    }
    if (insert(b.ids[r], values, std::span<const bool>(isnull.get(), ncols)) !=
        0) {
      return -1;
    }
  }
  b.ids.clear();
  for (size_t c = 0; c < ncols; c++) {
    b.vals[c].clear();
    b.nulls[c].clear();
  }
  return 0;
}

/* Open the row group part of a scan; see colstore.h. */
int col_scan_open(table_t *table, std::vector<bool> cols,
                  std::vector<col_bound_t> bounds, uint64_t id_lo,
                  uint64_t id_hi, col_scan_t *&out) {
  out = nullptr;
  std::unique_ptr<col_scan_t> c(new col_scan_t);
  c->table = table;
  c->cols = std::move(cols);
  c->bounds = std::move(bounds);
  c->id_lo = id_lo;
  c->id_hi = id_hi;
  const table_def_t &def = tbl_def(table);
  c->values.assign(def.columns.size(), 0);
  c->isnull.reset(new bool[def.columns.size()]);
  // The row groups the transaction sees: their RG keys alone.
  if (sm1::fileset_list(tbl_session(table)->sm1, def.storeid, c->rgs) != 0) {
    return -1;
  }
  out = c.release();
  return 0;
}

namespace {

/*
 * Whether row group rg_id, with metadata meta, may hold a row of
 * scan c: its _id range, then each bound against its CG's zonemap,
 * and its filter for an equality or IN (../COLSTORE.md §6.2).
 * 1, 0, or -1.
 */
int may_hold(col_scan_t *c, uint64_t rg_id, const rg_meta_t &meta) {
  if (meta.id_max < c->id_lo || meta.id_min > c->id_hi) {
    return 0;
  }
  const table_def_t &def = tbl_def(c->table);
  sm1::session_t *s = tbl_session(c->table)->sm1;
  for (const col_bound_t &b : c->bounds) {
    const rg_col_t *col = nullptr;
    for (const rg_col_t &x : meta.cols) {
      if (x.attnum == b.attnum) {
        col = &x;
      }
    }
    // A column added since the row group was sealed is its missing
    // value in every row: not pruned here.
    if (!col || b.attnum < 0 ||
        static_cast<size_t>(b.attnum) >= def.columns.size()) {
      continue;
    }
    const column_def_t &cd = def.columns[b.attnum];
    sm2::cg_column_t cc{cd.type, cd.typmod, cd.collation};
    // The filter is read only for a test it can answer.
    std::string_view filter;
    std::string held;
    bool eq = b.bound.has_in ||
              (b.bound.has_lo && b.bound.has_hi && b.bound.lo == b.bound.hi);
    if (eq) {
      std::string_view d;
      int r = sm1::read(s, def.storeid, filter_key(rg_id, b.attnum), d);
      if (r < 0) {
        return -1;
      }
      if (r == 0) {
        held.assign(d.data(), d.size());
        filter = held;
      }
    }
    if (!sm2::cg_may_match(cc, col->stats, filter, b.bound)) {
      return 0;
    }
  }
  return 1;
}

} // namespace

/* The next row of the row groups; see colstore.h. */
int col_scan_next(col_scan_t *c, uint64_t &id, std::string_view &tuple) {
  const table_def_t &def = tbl_def(c->table);
  const size_t ncols = def.columns.size();
  for (;;) {
    if (!c->open) {
      // The next row group that may hold a row.
      if (c->next >= c->rgs.size()) {
        return 1;
      }
      auto &[rg_id, value] = c->rgs[c->next++];
      rg_meta_t meta;
      if (rg_meta_decode(value, meta) != 0) {
        return -1;
      }
      int m = may_hold(c, rg_id, meta);
      if (m < 0) {
        return -1;
      }
      if (m == 0) {
        continue;
      }
      if (rg_read_open(c->table, rg_id, std::move(meta), c->cols, c->rg) != 0) {
        return -1;
      }
      c->open = true;
      // A row group a fifth dead is queued for its compaction: a
      // restart loses the markers' counts (../COLSTORE.md §11.1).
      if (c->rg.deleted.size() * 5 >= c->rg.meta.rows) {
        sealer_enqueue_rg(tbl_session(c->table)->cat, tbl_def(c->table).oid,
                          rg_id);
      }
    }
    int r = rg_read_next(c->table, c->rg, id, c->values,
                         std::span<bool>(c->isnull.get(), ncols));
    if (r < 0) {
      return -1;
    }
    if (r == 1) {
      rg_read_close(c->rg);
      c->open = false;
      continue;
    }
    if (id < c->id_lo || id > c->id_hi) {
      continue;
    }
    if (sm2::tup_encode(def.desc, c->values,
                        std::span<const bool>(c->isnull.get(), ncols),
                        c->tuple) != 0) {
      return -1;
    }
    tuple = c->tuple;
    return 0;
  }
}

/* The row group of row id's live copy, newest committed; see colstore.h. */
int col_find_latest(table_t *table, uint64_t id, uint64_t &rg_id) {
  rg_id = 0;
  sm1::session_t *s = tbl_session(table)->sm1;
  // Read the newest committed state, then the view as it was.
  bool was = false;
  if (sm1::view_latest(s, true, &was) != 0) {
    return -1;
  }
  int rc = 1;
  col_scan_t *c = nullptr;
  if (col_scan_open(table, {false}, {}, id, id, c) != 0) {
    rc = -1;
  } else {
    // No column read but _id: a row is its row group's.
    uint64_t got;
    std::string_view tuple;
    int r = col_scan_next(c, got, tuple);
    if (r < 0) {
      rc = -1;
    } else if (r == 0) {
      rg_id = col_scan_rg(c);
      rc = 0;
    }
    col_scan_close(c);
  }
  sm1::view_latest(s, was);
  return rc;
}

/* Compact row groups; see colstore.h. */
int col_compact(table_t *table, std::span<const uint64_t> rgs, bool &done) {
  done = false;
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  // 1. A savepoint, so a give-up leaves nothing; the newest committed
  //    state, so no delete committed since a snapshot is missed.
  int depth = sm1::savepoint(s);
  if (depth < 0) {
    return -1;
  }
  bool was = false;
  if (sm1::view_latest(s, true, &was) != 0) {
    return -1;
  }
  auto give_up = [&](int rc) {
    sm1::view_latest(s, was);
    if (sm1::rollback(s, depth) != 0) {
      return -1;
    }
    return rc;
  };
  std::vector<std::pair<uint64_t, std::string>> sets;
  if (sm1::fileset_list(s, def.storeid, sets) != 0) {
    return give_up(-1);
  }
  rg_build_t b;
  rg_build_begin(b, table);
  const size_t ncols = def.columns.size();
  std::vector<sm2::datum_t> values(ncols);
  std::unique_ptr<bool[]> isnull(new bool[ncols]);
  std::vector<rg_meta_t> olds;
  for (uint64_t rg_id : rgs) {
    const std::string *value = nullptr;
    for (const auto &[id, v] : sets) {
      if (id == rg_id) {
        value = &v;
      }
    }
    if (!value) {
      return give_up(
          sm1::seterr(sm1::SM_ERR_NOTFOUND,
                      "column store: no row group " + std::to_string(rg_id)));
    }
    rg_meta_t meta;
    if (rg_meta_decode(*value, meta) != 0) {
      return give_up(-1);
    }
    olds.push_back(meta);
    rg_read_t r;
    if (rg_read_open(table, rg_id, std::move(meta), {}, r) != 0) {
      rg_read_close(r);
      return give_up(-1);
    }
    // 2. Each live row: its lock, its moved marker, its values.
    uint64_t id;
    int rc;
    while ((rc = rg_read_next(table, r, id, values,
                              std::span<bool>(isnull.get(), ncols))) == 0) {
      int got = tbl_trylock(table, id, sm1::SM_LOCKMODE_EX);
      if (got != 0) {
        rg_read_close(r);
        return give_up(got < 0 ? -1 : 0);
      }
      if (sm1::insert_sealed(s, def.storeid, marker_key(rg_id, id), "") != 0 ||
          rg_build_add(b, id, values,
                       std::span<const bool>(isnull.get(), ncols)) != 0) {
        rg_read_close(r);
        return give_up(-1);
      }
      if (b.ids.size() >= rg_max_rows && rg_build_flush(b) != 0) {
        rg_read_close(r);
        return give_up(-1);
      }
    }
    rg_read_close(r);
    if (rc < 0) {
      return give_up(-1);
    }
  }
  // 3. The new row group, then the old ones' keys and filters.
  if (rg_build_flush(b) != 0) {
    return give_up(-1);
  }
  for (size_t i = 0; i < olds.size(); i++) {
    uint64_t rg_id = rgs[i];
    if (sm1::fileset_erase(s, def.storeid, rg_id) != 0) {
      return give_up(-1);
    }
    for (const rg_col_t &c : olds[i].cols) {
      if (sm1::erase(s, def.storeid, filter_key(rg_id, c.attnum)) != 0) {
        return give_up(-1);
      }
    }
  }
  sm1::view_latest(s, was);
  done = true;
  return sm1::forget(s, depth);
}

/* Close the row group part of a scan; see colstore.h. */
void col_scan_close(col_scan_t *c) {
  if (!c) {
    return;
  }
  rg_read_close(c->rg);
  delete c;
}

} // namespace sm3
