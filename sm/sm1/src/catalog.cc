/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * catalog.cc -- stores: lookup, sm1::mkstore, sm1::rmstore,
 * sm1::lsstore (../DBMGR.md §7).
 *
 * The catalog is storeid 0, an ordinary MVCC store keyed
 * "R" | be64(storeid) with an empty value, so DDL is
 * transactional by going through the ordinary write path. The
 * row records that the store exists; the storeid itself names
 * the column family, CF:store.<storeid>. The design's created_ts
 * field is left out: nothing reads it.
 *
 * sm1::mkstore allocates the storeid from the lease in CF:meta
 * (../XCTMGR.md §2.3), so a storeid is never reused and never names two
 * stores.
 *
 * A session caches what each lookup found, trusted at the snapshot
 * it was found at, and at another while no store was made or dropped
 * between the two (last_ddl_ts, ../DBMGR.md §7). A committed sm1::rmstore
 * leaves its column family in place until the watermark passes the drop and GC
 * removes it
 * (../GC.md §1).
 */

#include "error.h"
#include "log.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"

#include <algorithm>
#include <chrono>
#include <new>
#include <thread>
#include <vector>

using namespace sm1_internal;

namespace {

/* The entries a session's store cache holds before it starts over. */
constexpr size_t store_cache_max = 4096;

/* The catalog key of storeid: "R" | be64(storeid). */
std::string catalog_key(uint64_t storeid) {
  std::string key("R");
  sm0::put_be64(key, storeid);
  return key;
}

/* sm1::mkstore, minus the out-of-memory guard. */
int mkstore(sm1::session_t *s, uint64_t *out, bool temporary, bool chained) {
  // Require the out-parameter.
  if (!out) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_mkstore: storeid is NULL");
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  uint64_t storeid;
  // Refuse writes in a read-only transaction, which may still
  // make a temporary store, then allocate the storeid: before the
  // CF is created.
  if ((!temporary && xct_check_readonly(s) != 0) ||
      db_alloc_objid(s->db, &storeid) != 0) {
    return -1;
  }
  // X on a new permanent store, to the transaction's end: an abort
  // drops it, and work outside a transaction passes over it
  // (sm1::store_work_begin). The storeid is fresh, so no one holds it.
  // A temporary store takes no lock: its drops take X.
  if (!temporary && lock_store(s, storeid, sm1::SM_LOCKMODE_EX, true) != 0) {
    return -1;
  }

  // Create the column family eagerly, before the catalog row,
  // so this transaction can write into it at once (../DBMGR.md §7).
  if (db_create_store_cf(s->db, storeid, temporary ? s : nullptr, chained) !=
      0) {
    return -1;
  }
  // Remember the CF, so abort or rollback drops it.
  s->created.push_back(storeid);
  // A temporary store is the session's, and has no catalog row
  // (TEMPTABLE.md §3.2).
  if (temporary) {
    s->temp_stores.insert(storeid);
    s->temp_touched = true;
    *out = storeid;
    return 0;
  }

  // Insert the catalog row through the ordinary write path. The
  // storeid is fresh, so no other transaction can be writing it.
  if (access_write(s, 0, WRITE_INSERT, catalog_key(storeid), rocksdb::Slice(),
                   nullptr) != 0) {
    // A retryable error already aborted the transaction.
    if (!s->active) {
      return -1; // retryable: the abort already dropped the CF
    }
    // Undo the eager CF creation.
    s->created.pop_back();
    db_drop_store_cf(s->db, storeid);
    return -1;
  }
  *out = storeid;
  return 0;
}

/* sm1::rmstore, minus the out-of-memory guard. */
int rmstore(sm1::session_t *s, uint64_t storeid, bool locked) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject storeid 0, the catalog's own slot.
  if (storeid == 0) {
    return set_error(sm1::SM_ERR_INVALID, "storeid 0 is not a store");
  }
  // Resolve the storeid at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), storeid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a storeid that is not live.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  // A load open into the store would ingest into a dropped one.
  if (load_open_on(s, storeid)) {
    return set_error(sm1::SM_ERR_INVALID, "a bulk load is open into the store");
  }
  // A temporary store has no catalog row: it goes at commit.
  if (s->temp_stores.count(storeid)) {
    // X, so its drop meets no work on it (sm1::store_work_begin).
    if (lock_store(s, storeid, sm1::SM_LOCKMODE_EX, false) != 0) {
      return -1;
    }
    s->dropped.push_back(storeid);
    s->temp_touched = true;
    return 0;
  }
  // Delete the catalog row; the column family stays until no
  // snapshot can still read the store (../GC.md §1).
  if (access_write(s, 0, WRITE_DELETE, catalog_key(storeid), rocksdb::Slice(),
                   nullptr) != 0) {
    return -1;
  }
  // Remember the storeid, so GC drops its CF after commit; under the
  // caller's lock, sm1::finish_drops drops it at once.
  s->dropped.push_back(storeid);
  if (locked) {
    s->dropped_locked.push_back(storeid);
  }
  // X, after the catalog row's lock, so a second drop still waits on
  // the row: work on the store that began before ends first, and none
  // begins from now (sm1::store_work_begin).
  return lock_store(s, storeid, sm1::SM_LOCKMODE_EX, false);
}

/* sm1::lsstore, minus the out-of-memory guard. */
int lsstore(sm1::session_t *s, std::vector<uint64_t> &out) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Scan every catalog row, "R" up to but excluding "S"; under
  // SERIALIZABLE this records the whole catalog as read.
  sm1::scan_t *c = access_scan(s, 0, xct_view(s), "R", 1, 1, "S", 1, 0);
  if (!c) {
    return -1;
  }
  // Close the cursor however the scan ends.
  defer { sm1::scanend(c); };

  std::vector<uint64_t> ids;
  std::string_view key, data;
  int r;
  // Collect the storeid of each live catalog row, in order.
  while ((r = sm1::scannxt(c, key, data)) == 0) {
    // Reject a malformed catalog key.
    if (key.size() != 9) {
      return set_error(sm1::SM_ERR_CORRUPT, "bad catalog key of {} bytes",
                       key.size());
    }
    ids.push_back(sm0::get_be64(key.data() + 1));
  }
  // Fail on a scan error.
  if (r < 0) {
    return -1;
  }
  // Hand the storeids to the caller.
  out = std::move(ids);
  return 0;
}

} // namespace

namespace sm1_internal {

/*
 * A point read of the catalog row at view, or the session's store
 * cache when no store was made or dropped since the entry's
 * snapshot nor the view's (../DBMGR.md §7). A temporary store has none: it
 * is live to the session that holds it, until that session drops
 * it.
 */
int catalog_lookup(sm1::session_t *s, const view_t &view, uint64_t storeid) {
  if (session_store(s, storeid).owner) {
    bool live = s->temp_stores.count(storeid) &&
                std::find(s->dropped.begin(), s->dropped.end(), storeid) ==
                    s->dropped.end();
    return live ? 0 : 1;
  }
  // A transaction that made or dropped a store reads its own
  // catalog versions, which no entry may stand for.
  bool own_ddl = !s->created.empty() || !s->dropped.empty();
  if (!own_ddl) {
    auto it = s->store_cache.find(storeid);
    if (it != s->store_cache.end()) {
      const sm1::session_t::store_seen_t &e = it->second;
      uint64_t ddl_ts = s->db->last_ddl_ts.load();
      // The one snapshot gives one answer whatever committed since,
      // or no DDL lies between the two; and under SERIALIZABLE only
      // if this transaction's own read took the SIREAD.
      bool same = e.snapshot_ts == view.snapshot_ts ||
                  (ddl_ts <= e.snapshot_ts && ddl_ts <= view.snapshot_ts);
      if (same && (!s->ssi || e.begin_seq == s->begin_seq)) {
        return e.live ? 0 : 1;
      }
    }
  }
  std::string val;
  // Read the catalog row at view: 0 if the store is live.
  int r = access_read(s, 0, view, catalog_key(storeid), &val);
  if (r >= 0 && !own_ddl) {
    // Start over rather than grow without bound.
    if (s->store_cache.size() >= store_cache_max) {
      s->store_cache.clear();
    }
    s->store_cache[storeid] = {view.snapshot_ts, s->begin_seq, r == 0};
  }
  return r;
}

} // namespace sm1_internal

/* Create a store and hand out its storeid. */
int sm1::mkstore(sm1::session_t *session, uint64_t &storeid, bool temporary,
                 bool chained) {
  return ::mkstore(session, &storeid, temporary, chained);
}

/* Drop a store and all of its rows. */
int sm1::rmstore(sm1::session_t *session, uint64_t storeid, bool locked) {
  return ::rmstore(session, storeid, locked);
}

/*
 * Drop the stores the session's commits dropped under a lock, then
 * wait for their files; see sm1.h.
 * 1. Drop each store's column family under X, and take its handle's
 *    flag. The commit let go of the drop's X, and the store stays
 *    registered until now, so a GC run may have taken IX on it to write
 *    to it (gc_store_try): X waits for that run to end, and a run after
 *    finds the store gone. A write to a dropped column family fails, and
 *    RocksDB fails the whole write group with it, a commit record's too.
 *    The X is taken in a transaction of the session's own for the
 *    locks, as a session's close does for its temporary stores.
 * 2. Erase the session's own cached handles of them.
 * 3. Wait until each handle's last copy is destroyed: RocksDB deletes
 *    a dropped column family's files then, in that thread.
 */
int sm1::finish_drops(sm1::session_t *session) {
  if (nullptr == session) {
    return set_error(sm1::SM_ERR_INVALID, "finish_drops: session is NULL");
  }
  std::vector<std::pair<uint64_t, std::shared_ptr<std::atomic<bool>>>> due;
  due.swap(session->drops_due);
  int rc = 0;
  // 1. Drop each one under X: no transaction reads it, as the caller's
  //    lock kept readers out until the commit.
  bool own = !due.empty() && !session->active &&
             sm1::begin(session, sm1::xctlevel_t::read_committed, true) == 0;
  std::vector<std::pair<uint64_t, std::shared_ptr<std::atomic<bool>>>> waits;
  for (auto &[storeid, flag] : due) {
    std::shared_ptr<std::atomic<bool>> gone;
    if (session->active) {
      lock_store(session, storeid, sm1::SM_LOCKMODE_EX, false);
    }
    if (db_drop_store_cf(session->db, storeid, &gone) != 0) {
      rc = -1;
    }
    // The flag the commit took, if the GC dropped the store first.
    if (!gone) {
      gone = std::move(flag);
    }
    if (gone) {
      waits.emplace_back(storeid, std::move(gone));
    }
  }
  // The locks go before the wait for the files: no one waits on them.
  if (own) {
    sm1::abort(session);
  }
  // 2. The session's own copies.
  for (const auto &[storeid, flag] : due) {
    session->store_handles.erase(storeid);
  }
  // 3. Another copy is held for a moment: a sweep's compaction of the
  //    store, or statistics. A wait of more than 10 s is logged once.
  for (const auto &[storeid, gone] : waits) {
    auto start = std::chrono::steady_clock::now();
    bool warned = false;
    while (!gone->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (!warned &&
          std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
        log_msg(sm1::loglevel_t::warn,
                "database {}: the drop of store {} waits for a copy of its "
                "handle",
                session->db->name.c_str(), storeid);
        warned = true;
      }
    }
  }
  return rc;
}

/* List the stores visible to the transaction. */
int sm1::lsstore(sm1::session_t *session, std::vector<uint64_t> &out) {
  return ::lsstore(session, out);
}
