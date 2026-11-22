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
#include "sm0_defer.h"

#include <cstring>

namespace {

/* The version of an RG key's value. */
constexpr uint32_t rg_meta_version = 2;

/* Fail with SM_ERR_CORRUPT: what is wrong with a row group. */
int corrupt(const std::string &what) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "column store: " + what);
}

/* Append v as two bytes, big-endian, as a relid is. */
void put_be16(std::string &k, uint16_t v) {
  k.push_back(static_cast<char>(v >> 8));
  k.push_back(static_cast<char>(v & 0xFF));
}

/* The key of a filter: relid, fsid, and the column, _id's 0. */
std::string filter_key(uint64_t fsid, int32_t attnum) {
  std::string k;
  put_be16(k, sm3::filter_relid);
  sm0::put_be64(k, fsid);
  put_be16(k, static_cast<uint16_t>(attnum + 1));
  return k;
}

/* The bounds of the keys of file set fsid's delete markers. */
std::string marker_bound(uint64_t fsid, unsigned char fill) {
  std::string k;
  put_be16(k, sm3::marker_relid);
  sm0::put_be64(k, fsid);
  k.append(4, static_cast<char>(fill));
  return k;
}

/* The length of a delete marker's key: relid, fsid and rno. */
constexpr size_t marker_key_len = 2 + 8 + 4;

} // namespace

/* The key of a row's delete marker; see colstore.h. */
std::string sm3::marker_key(place_t place) {
  std::string k;
  put_be16(k, sm3::marker_relid);
  sm0::put_be64(k, place.fsid);
  sm0::put_be32(k, place.rno);
  return k;
}

/* The key of a file set's validmap; see colstore.h. */
std::string sm3::validmap_key(uint64_t fsid) {
  std::string k;
  put_be16(k, sm3::validmap_relid);
  sm0::put_be64(k, fsid);
  return k;
}

/* The lock-table name of a row group; see colstore.h. */
std::string sm3::rg_lock_key(uint64_t rg_id) {
  std::string k;
  put_be16(k, sm3::rglock_relid);
  sm0::put_be64(k, rg_id);
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
    std::string st;
    sm2::cg_stats_encode(c.stats, st);
    put_str(out, st);
  }
  sm0::put_be32(out, meta.rows0);
  put_str(out, meta.dropped);
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
    if (in.size() < 20) {
      return corrupt("an RG key's column is cut short");
    }
    rg_col_t c;
    c.attnum = static_cast<int32_t>(sm0::get_be32(in.data()));
    c.size = sm0::get_be64(in.data() + 4);
    c.crc = sm0::get_be32(in.data() + 12);
    uint32_t len = sm0::get_be32(in.data() + 16);
    in.remove_prefix(20);
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
  if (in.size() < 8) {
    return corrupt("an RG key's dropped rows are cut short");
  }
  meta.rows0 = sm0::get_be32(in.data());
  uint32_t dl = sm0::get_be32(in.data() + 4);
  in.remove_prefix(8);
  if (in.size() < dl) {
    return corrupt("an RG key's dropped rows are cut short");
  }
  meta.dropped.assign(in.data(), dl);
  return 0;
}

/* Map an rno to another version; see colstore.h. */
bool rg_map_rno(const rg_meta_t &from, uint32_t rno, const rg_meta_t &to,
                uint32_t &out) {
  // 1. To version 0: the rno-th row of version 0 not in from's
  //    dropped set.
  uint32_t rno0 = rno;
  if (from.rows0 != 0) {
    uint32_t k = 0;
    rno0 = UINT32_MAX;
    for (uint32_t i = 0; i < from.rows0; i++) {
      if (validmap_test(from.dropped, i)) {
        continue;
      }
      if (k++ == rno) {
        rno0 = i;
        break;
      }
    }
    if (rno0 == UINT32_MAX) {
      return false;
    }
  }
  // 2. Not in to, if to dropped it; else past to's dropped rows below.
  if (to.rows0 == 0) {
    out = rno0;
    return true;
  }
  if (validmap_test(to.dropped, rno0)) {
    return false;
  }
  uint32_t below = 0;
  for (uint32_t i = 0; i < rno0; i++) {
    below += validmap_test(to.dropped, i) ? 1 : 0;
  }
  out = rno0 - below;
  return true;
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
    // A value out of line is read back, and made a varlena again, as
    // tbl_fetch gives its bytes with no header: a CG holds no TOAST
    // pointer (../COLSTORE.md §5).
    if (c.type->len < 0 && sm2::varlena_is_toast(bytes)) {
      std::string full;
      if (tbl_fetch(b.table, bytes, full) != 0) {
        return -1;
      }
      b.vals[i].push_back(sm2::make_varlena(full));
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
  //    Its first version is 0 (../COLSTORE2.md §3). A compaction
  //    names the next version of its row group instead.
  uint64_t fsid = b.fsid;
  if (0 == fsid) {
    uint64_t rg_id = 0;
    if (sm1::nextseq(s, rg_seq_name(def.oid), rg_id) != 0) {
      return -1;
    }
    fsid = fsid_of(rg_id, 0);
  }
  rg_meta_t meta;
  meta.rows = static_cast<uint32_t>(n);
  meta.rows0 = b.rows0;
  meta.dropped = b.dropped;
  meta.id_min = UINT64_MAX;
  // 2. The CG of each column, _id's first, and its filter key.
  auto write_cg = [&](int32_t attnum, const sm2::cg_column_t &col,
                      const std::vector<sm2::cg_value_t> &v) -> int {
    sm2::cg_built_t built;
    if (sm2::cg_build(col, v, built) != 0) {
      return -1;
    }
    std::string name = rg_file_name(attnum);
    if (sm1::fileset_write(s, def.storeid, fsid, name, built.file) != 0) {
      return -1;
    }
    rg_col_t c;
    c.attnum = attnum;
    c.size = built.file.size();
    c.crc = sm0::crc32c(built.file);
    c.stats = std::move(built.stats);
    if (sm1::insert_new(s, def.storeid, filter_key(fsid, attnum),
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
  if (sm1::fileset_put(s, def.storeid, fsid, payload) != 0) {
    return -1;
  }
  // 4. The next row group starts empty, and new.
  b.fsid = 0;
  b.rows0 = 0;
  b.dropped.clear();
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
int rg_read_open(table_t *table, uint64_t fsid, rg_meta_t meta,
                 const std::vector<bool> &cols, rg_read_t &r) {
  rg_read_close(r);
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  r.fsid = fsid;
  r.meta = std::move(meta);
  r.cgs.assign(def.columns.size(), nullptr);
  std::vector<bool> in_rg(def.columns.size(), false);
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
    if (sm1::fileset_read(s, def.storeid, fsid, rg_file_name(c.attnum), 0,
                          UINT64_MAX, file) != 0) {
      return -1;
    }
    if (file.size() != c.size || sm0::crc32c(file) != c.crc) {
      return corrupt("file set " + std::to_string(fsid) + "'s CG " +
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
      in_rg[c.attnum] = true;
    }
  }
  // 2. Each column wanted that has no CG here, added since the row
  //    group was written: its missing value in every row, or null
  //    (../COLSTORE.md §14).
  r.added.assign(def.columns.size(), false);
  for (size_t a = 0; a < def.columns.size(); a++) {
    const column_def_t &cd = def.columns[a];
    r.added[a] = !in_rg[a] && !cd.dropped && cd.missing.has_value() &&
                 (cols.empty() || (a < cols.size() && cols[a]));
  }
  if (!r.ids) {
    return corrupt("file set " + std::to_string(fsid) + " has no _id CG");
  }
  // 3. Its rows deleted, at the transaction's view, as a bitmap by
  //    rno: its validmap's, and its markers' (../COLSTORE2.md §6.2).
  return col_dead(table, fsid, sm2::cg_rows(r.ids), r.deleted, r.ndeleted,
                  r.nmarkers);
}

/* A file set's rows deleted; see colstore.h. */
int col_dead(table_t *table, uint64_t fsid, uint32_t rows,
             std::vector<bool> &dead, uint32_t &ndead, uint32_t &markers) {
  dead.assign(rows, false);
  ndead = 0;
  markers = 0;
  // 1. The validmap, if a fold wrote one.
  std::string_view bits;
  int r = sm1::read(tbl_session(table)->sm1, tbl_def(table).storeid,
                    validmap_key(fsid), bits);
  if (r < 0) {
    return -1;
  }
  if (r == 0) {
    for (uint32_t i = 0; i < rows; i++) {
      if (validmap_test(bits, i)) {
        dead[i] = true;
        ndead++;
      }
    }
  }
  // 2. The markers not folded yet.
  std::vector<uint32_t> rnos;
  if (col_markers(table, fsid, rnos) != 0) {
    return -1;
  }
  for (uint32_t rno : rnos) {
    if (rno < rows && !dead[rno]) {
      dead[rno] = true;
      ndead++;
      markers++;
    }
  }
  return 0;
}

/* A file set's delete markers; see colstore.h. */
int col_markers(table_t *table, uint64_t fsid, std::vector<uint32_t> &rnos) {
  std::string lo = marker_bound(fsid, 0x00), hi = marker_bound(fsid, 0xFF);
  sm1::scan_t *c = nullptr;
  if (sm1::scan(tbl_session(table)->sm1, tbl_def(table).storeid, lo.data(),
                lo.size(), 1, hi.data(), hi.size(), 1, c) != 0) {
    return -1;
  }
  std::string_view k, d;
  int rc;
  while ((rc = sm1::scannxt(c, k, d)) == 0) {
    if (k.size() == marker_key_len) {
      rnos.push_back(sm0::get_be32(k.data() + 10));
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
  for (const auto &[fsid, value] : rgs) {
    rg_meta_t meta;
    if (rg_meta_decode(value, meta) != 0) {
      return -1;
    }
    for (const rg_col_t &c : meta.cols) {
      bytes += c.size;
    }
  }
  return 0;
}

/* The next live row of a row group; see colstore.h. */
int rg_read_next(table_t *table, rg_read_t &r, uint64_t &id,
                 std::span<sm2::datum_t> values, std::span<bool> isnull) {
  const table_def_t &def = tbl_def(table);
  for (size_t c = 0; c < values.size(); c++) {
    values[c] = 0;
    isnull[c] = true;
  }
  while (r.at < sm2::cg_rows(r.ids)) {
    uint32_t i = r.at++;
    std::string_view d;
    if (sm2::cg_get(r.ids, i, d) != 0 || d.size() != 8) {
      return corrupt("an _id CG's row is not an _id");
    }
    uint64_t row_id;
    memcpy(&row_id, d.data(), 8);
    // A row its delete marker names is gone from this row group.
    if (i < r.deleted.size() && r.deleted[i]) {
      continue;
    }
    for (size_t c = 0; c < def.columns.size(); c++) {
      if (!r.cgs[c]) {
        if (r.added[c]) {
          values[c] = to_datum(def.columns[c].type, *def.columns[c].missing);
          isnull[c] = false;
        }
        continue;
      }
      if (sm2::cg_get(r.cgs[c], i, d) == 1) {
        continue;
      }
      values[c] = to_datum(def.columns[c].type, d);
      isnull[c] = false;
    }
    id = row_id;
    r.last = i;
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
 * Whether the row group of file set fsid, with metadata meta, may
 * hold a row of scan c: its _id range, then each bound against its
 * CG's zonemap, and its filter for an equality or IN
 * (../COLSTORE.md §6.2). 1, 0, or -1.
 */
int may_hold(col_scan_t *c, uint64_t fsid, const rg_meta_t &meta) {
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
      int r = sm1::read(s, def.storeid, filter_key(fsid, b.attnum), d);
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
      auto &[fsid, value] = c->rgs[c->next++];
      rg_meta_t meta;
      if (rg_meta_decode(value, meta) != 0) {
        return -1;
      }
      int m = may_hold(c, fsid, meta);
      if (m < 0) {
        return -1;
      }
      if (m == 0) {
        continue;
      }
      if (rg_read_open(c->table, fsid, std::move(meta), c->cols, c->rg) != 0) {
        return -1;
      }
      c->open = true;
      // A row group a fifth dead is queued for its compaction, and
      // one whose markers are 1% of its rows for its fold: a restart
      // loses the markers' counts (../COLSTORE.md §11.1,
      // ../COLSTORE2.md §6.10).
      const catalog_t *cat = tbl_session(c->table)->cat;
      if (static_cast<uint64_t>(c->rg.ndeleted) * 5 >= c->rg.meta.rows) {
        sealer_enqueue_rg(cat, tbl_def(c->table).oid, fsid);
      } else if (c->rg.nmarkers > 0 &&
                 static_cast<uint64_t>(c->rg.nmarkers) * 100 >=
                     c->rg.meta.rows) {
        sealer_enqueue_fold(cat, tbl_def(c->table).oid, fsid);
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

namespace {

/* The place of row id's live copy: newest committed with latest. */
int find_row(table_t *table, uint64_t id, bool latest, place_t &place) {
  place = place_t();
  sm1::session_t *s = tbl_session(table)->sm1;
  // Read the newest committed state, then the view as it was.
  bool was = false;
  if (latest && sm1::view_latest(s, true, &was) != 0) {
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
      place = col_scan_place(c);
      rc = 0;
    }
    col_scan_close(c);
  }
  if (latest) {
    sm1::view_latest(s, was);
  }
  return rc;
}

} // namespace

/* The place of row id's live copy, newest committed; see colstore.h. */
int col_find_latest(table_t *table, uint64_t id, place_t &place) {
  return find_row(table, id, true, place);
}

/* The place of row id's live copy, as seen; see colstore.h. */
int col_find_seen(table_t *table, uint64_t id, place_t &place) {
  return find_row(table, id, false, place);
}

/* The newest version of a row group; see colstore.h. */
int col_newest_version(table_t *table, uint64_t rg_id, uint64_t &fsid,
                       rg_meta_t &meta) {
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  bool was = false;
  if (sm1::view_latest(s, true, &was) != 0) {
    return -1;
  }
  defer { sm1::view_latest(s, was); };
  // 1. The version named on entry, by its _id CG's filter key, which
  //    lives as long as its RG key: one read.
  if (0 != fsid && fsid_rg(fsid) == rg_id) {
    std::string_view d;
    int r = sm1::read(s, def.storeid, filter_key(fsid, -1), d);
    if (r < 0) {
      return -1;
    }
    if (r == 0) {
      return col_meta_seen(table, fsid, meta);
    }
  }
  // 2. Else each file set, for the row group's.
  std::vector<std::pair<uint64_t, std::string>> sets;
  if (sm1::fileset_list(s, def.storeid, sets) != 0) {
    return -1;
  }
  for (const auto &[id, v] : sets) {
    if (fsid_rg(id) == rg_id) {
      fsid = id;
      return rg_meta_decode(v, meta);
    }
  }
  return 1;
}

/* A file set's metadata as the transaction sees it; see colstore.h. */
int col_meta_seen(table_t *table, uint64_t fsid, rg_meta_t &meta) {
  std::vector<std::pair<uint64_t, std::string>> sets;
  if (sm1::fileset_list(tbl_session(table)->sm1, tbl_def(table).storeid,
                        sets) != 0) {
    return -1;
  }
  for (const auto &[id, v] : sets) {
    if (id == fsid) {
      return rg_meta_decode(v, meta);
    }
  }
  return 1;
}

namespace {

/*
 * A row group job on one version, compaction's or elimination's, in
 * the caller's transaction at the newest committed state: a savepoint,
 * so a give-up leaves nothing, and X on the row group.
 */
struct rg_job_t {
  explicit rg_job_t(table_t *t) : table(t), s(tbl_session(t)->sm1) {}

  table_t *table;
  sm1::session_t *s;
  int depth = -1;
  bool was = false;
  rg_meta_t meta;

  /* Give up with rc: the view as it was, and nothing left written. */
  int give_up(int rc) {
    sm1::view_latest(s, was);
    if (depth >= 0 && sm1::rollback(s, depth) != 0) {
      return -1;
    }
    return rc;
  }

  /*
   * Begin on file set fsid:
   * 1. Set a savepoint, and read the newest committed state.
   * 2. Take X on the row group, if no writer holds IX.
   * 3. Read the version's RG key: gone is SM_ERR_NOTFOUND.
   * Returns 0 to go on; 1, given up; or -1, given up.
   */
  int begin(uint64_t fsid) {
    const table_def_t &def = tbl_def(table);
    // 1. A savepoint; the newest committed state.
    depth = sm1::savepoint(s);
    if (depth < 0) {
      return -1;
    }
    if (sm1::view_latest(s, true, &was) != 0) {
      return -1;
    }
    // 2. X on the row group, NOWAIT: the sealer waits for no writer.
    int got = sm1::trylock_row(s, def.storeid, rg_lock_key(fsid_rg(fsid)),
                               sm1::SM_LOCKMODE_EX, false, rglock_relid);
    if (got != 0) {
      return got < 0 ? give_up(-1) : give_up(0) == 0 ? 1 : -1;
    }
    // 3. The version.
    int f = col_meta_seen(table, fsid, meta);
    if (f != 0) {
      return give_up(f < 0 ? -1
                           : sm1::seterr(sm1::SM_ERR_NOTFOUND,
                                         "column store: no file set " +
                                             std::to_string(fsid)));
    }
    return 0;
  }

  /*
   * End on file set fsid: erase its RG key and filter keys; its
   * markers and validmap stay for the snapshots that see it. The
   * savepoint goes. Returns 0 or -1.
   */
  int end(uint64_t fsid) {
    const table_def_t &def = tbl_def(table);
    if (sm1::fileset_erase(s, def.storeid, fsid) != 0) {
      return give_up(-1);
    }
    for (const rg_col_t &c : meta.cols) {
      if (sm1::erase(s, def.storeid, filter_key(fsid, c.attnum)) != 0) {
        return give_up(-1);
      }
    }
    sm1::view_latest(s, was);
    return sm1::forget(s, depth);
  }
};

} // namespace

/*
 * Compact a version into its next; see colstore.h:
 * 1. Begin the job: X on the row group.
 * 2. Read the version's live rows. A row lock is on the row's _id,
 *    which the next version keeps, so a holder of one stays.
 * 3. The next version's dropped set: this one's, and its dead rows.
 * 4. Write the next version with the live rows alone, in their order.
 * 5. End the job: this version's RG key and filter keys erased.
 */
int col_compact(table_t *table, uint64_t fsid, bool &done) {
  done = false;
  if (fsid_version(fsid) >= rg_last_version) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "column store: file set " + std::to_string(fsid) +
                           " is a row group's last version");
  }
  // 1. X on the row group.
  rg_job_t j(table);
  int rc = j.begin(fsid);
  if (rc != 0) {
    return rc < 0 ? -1 : 0;
  }
  const rg_meta_t &meta = j.meta;
  // 2. The live rows.
  rg_read_t r;
  defer { rg_read_close(r); };
  if (rg_read_open(table, fsid, meta, {}, r) != 0) {
    return j.give_up(-1);
  }
  rg_build_t b;
  rg_build_begin(b, table);
  const size_t ncols = tbl_def(table).columns.size();
  std::vector<sm2::datum_t> values(ncols);
  std::unique_ptr<bool[]> isnull(new bool[ncols]);
  uint64_t id;
  while ((rc = rg_read_next(table, r, id, values,
                            std::span<bool>(isnull.get(), ncols))) == 0) {
    if (rg_build_add(b, id, values,
                     std::span<const bool>(isnull.get(), ncols)) != 0) {
      return j.give_up(-1);
    }
  }
  if (rc < 0) {
    return j.give_up(-1);
  }
  // 3. The dropped set, of version 0's rows.
  const uint32_t rows0 = meta.rows0 != 0 ? meta.rows0 : meta.rows;
  std::string dropped = meta.dropped;
  dropped.resize((rows0 + 7) / 8, '\0');
  for (uint32_t i = 0, k = 0; i < rows0; i++) {
    if (meta.rows0 != 0 && validmap_test(meta.dropped, i)) {
      continue;
    }
    if (k < r.deleted.size() && r.deleted[k]) {
      dropped[i / 8] = static_cast<char>(
          static_cast<unsigned char>(dropped[i / 8]) | (1u << (i % 8)));
    }
    k++;
  }
  // 4. The next version; none if no row is left.
  b.fsid = fsid + 1;
  b.rows0 = rows0;
  b.dropped = std::move(dropped);
  if (rg_build_flush(b) != 0) {
    return j.give_up(-1);
  }
  // 5. This version's keys.
  if (j.end(fsid) != 0) {
    return -1;
  }
  done = true;
  return 0;
}

/*
 * Eliminate a row group's newest version; see colstore.h:
 * 1. Begin the job: X on the row group.
 * 2. Read the version's live rows; try each row's lock, as the write
 *    of its delta key would wait for it: a holder makes it give up.
 * 3. Give each to movein, a moved-in version in the delta store.
 * 4. End the job: the version's RG key and filter keys erased.
 */
int col_eliminate(table_t *table, uint64_t fsid, const movein_fn &movein,
                  bool &done) {
  done = false;
  // 1. X on the row group.
  rg_job_t j(table);
  int rc = j.begin(fsid);
  if (rc != 0) {
    return rc < 0 ? -1 : 0;
  }
  // 2. The live rows, each one's lock tried; 3. each moved in.
  rg_read_t r;
  defer { rg_read_close(r); };
  if (rg_read_open(table, fsid, j.meta, {}, r) != 0) {
    return j.give_up(-1);
  }
  const size_t ncols = tbl_def(table).columns.size();
  std::vector<sm2::datum_t> values(ncols);
  std::unique_ptr<bool[]> isnull(new bool[ncols]);
  uint64_t id;
  while ((rc = rg_read_next(table, r, id, values,
                            std::span<bool>(isnull.get(), ncols))) == 0) {
    int lk = tbl_trylock(table, id, sm1::SM_LOCKMODE_EX);
    if (lk != 0) {
      return j.give_up(lk < 0 ? -1 : 0);
    }
    if (movein(id, values, std::span<const bool>(isnull.get(), ncols)) != 0) {
      return j.give_up(-1);
    }
  }
  if (rc < 0) {
    return j.give_up(-1);
  }
  // 4. The version's keys.
  if (j.end(fsid) != 0) {
    return -1;
  }
  done = true;
  return 0;
}

/* Erase an old version's markers and validmap; see colstore.h. */
int col_cleanup(table_t *table, uint64_t fsid, bool &done) {
  done = false;
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  // 1. Settled gone: no snapshot sees the version, so no reader reads
  //    its markers and no writer adds a shadow marker to them.
  sm1::rawpin_t *pin = sm1::rawpin_new();
  defer { sm1::rawpin_free(pin); };
  std::string_view d;
  int r =
      sm1::read_settled_versioned(s, def.storeid, filter_key(fsid, -1), pin, d);
  if (r != 1) {
    return r < 0 ? -1 : 0;
  }
  // 2. Its markers and its validmap, at the newest committed state.
  bool was = false;
  if (sm1::view_latest(s, true, &was) != 0) {
    return -1;
  }
  defer { sm1::view_latest(s, was); };
  std::vector<uint32_t> rnos;
  if (col_markers(table, fsid, rnos) != 0) {
    return -1;
  }
  for (uint32_t rno : rnos) {
    if (sm1::erase(s, def.storeid, marker_key({fsid, rno})) != 0) {
      return -1;
    }
  }
  r = sm1::read(s, def.storeid, validmap_key(fsid), d);
  if (r < 0 ||
      (r == 0 && sm1::erase(s, def.storeid, validmap_key(fsid)) != 0)) {
    return -1;
  }
  done = true;
  return 0;
}

/* Fold a file set's markers into its validmap; see colstore.h. */
int col_fold(table_t *table, uint64_t fsid, bool &done) {
  done = false;
  const table_def_t &def = tbl_def(table);
  sm1::session_t *s = tbl_session(table)->sm1;
  // 1. X on the row group, if no writer holds IX: no marker is then
  //    in flight, and each one read below is committed.
  int got = sm1::trylock_row(s, def.storeid, rg_lock_key(fsid >> 2),
                             sm1::SM_LOCKMODE_EX, false, rglock_relid);
  if (got != 0) {
    return got < 0 ? -1 : 0;
  }
  // 2. The newest committed state, then the view as it was.
  bool was = false;
  if (sm1::view_latest(s, true, &was) != 0) {
    return -1;
  }
  defer { sm1::view_latest(s, was); };
  std::vector<std::pair<uint64_t, std::string>> sets;
  if (sm1::fileset_list(s, def.storeid, sets) != 0) {
    return -1;
  }
  const std::string *value = nullptr;
  for (const auto &[id, v] : sets) {
    if (id == fsid) {
      value = &v;
    }
  }
  if (!value) {
    done = true;
    return 0;
  }
  rg_meta_t meta;
  if (rg_meta_decode(*value, meta) != 0) {
    return -1;
  }
  // 3. Its validmap and its markers.
  std::string bits;
  {
    std::string_view d;
    int r = sm1::read(s, def.storeid, validmap_key(fsid), d);
    if (r < 0) {
      return -1;
    }
    if (r == 0) {
      bits.assign(d.data(), d.size());
    }
  }
  const bool had = !bits.empty();
  std::vector<uint32_t> rnos;
  if (col_markers(table, fsid, rnos) != 0) {
    return -1;
  }
  if (rnos.empty()) {
    done = true;
    return 0;
  }
  // 4. A new validmap: the old bits, and a bit for each marker.
  bits.resize((meta.rows + 7) / 8, '\0');
  for (uint32_t rno : rnos) {
    if (rno < meta.rows) {
      bits[rno / 8] = static_cast<char>(
          static_cast<unsigned char>(bits[rno / 8]) | (1u << (rno % 8)));
    }
  }
  if ((had ? sm1::update(s, def.storeid, validmap_key(fsid), bits)
           : sm1::insert(s, def.storeid, validmap_key(fsid), bits)) != 0) {
    return -1;
  }
  // 5. The markers it holds now, erased.
  for (uint32_t rno : rnos) {
    if (sm1::erase(s, def.storeid, marker_key({fsid, rno})) != 0) {
      return -1;
    }
  }
  done = true;
  return 0;
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
