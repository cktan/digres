/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * sealer.cc -- the seal's job (../../COLSTORE.md §10.4): one worker
 * thread for each catalog, which seals the tables queued to it, a
 * row group at a time, each in a READ COMMITTED transaction of its
 * own. The insert path counts the rows put into a column store
 * table's delta store, and queues the table at rg_max_rows; a scan
 * that reads that many of them queues it too, for counts a restart
 * lost. The same worker folds a row group's delete markers into its
 * validmap, compacts a row group that delete markers committed on it
 * have made a fifth dead into its next version, eliminates one at its
 * last version or under half live, and cleans up the versions no
 * snapshot sees (../../COLSTORE.md §11.1 to §10.4).
 */

#include "colstore.h"

#include "sm0_arena.h"
#include "sm0_log.h"
#include "ty.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace sm3 {

namespace {

/* The kinds of a sealer's job. */
enum job_kind_t : int { job_seal = 0, job_compact = 1, job_fold = 2 };

/* A job: (table, 0, seal), or (table, fsid, compaction or fold). */
using job_t = std::tuple<uint64_t, uint64_t, int>;

/* A catalog's sealer: its queue, its counts, and its thread. */
struct sealer_t {
  catalog_t *cat = nullptr;
  std::function<sm1::session_t *()> open;
  std::mutex mu;
  std::condition_variable cv;
  /* Jobs, in order. */
  std::deque<job_t> queue;
  std::set<job_t> queued; /* the jobs in queue */
  /* Delete markers committed on a file set, by (table, fsid): since
     its last compaction's job, and since its last fold's. */
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> marks;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> fold_marks;
  /* Old versions to clean up, by (table, fsid), once no snapshot
     sees them (../../COLSTORE.md §11.4): tried each second. */
  std::set<std::pair<uint64_t, uint64_t>> cleanups;
  std::unordered_map<uint64_t, uint64_t> counts; /* delta rows put */
  bool stop = false;
  bool busy = false; /* a table is being sealed */
  std::thread thread;
};

/* Every attached catalog's sealer. */
std::mutex g_mu;
std::unordered_map<const catalog_t *, sealer_t *> g_sealers;

/* The sealer of cat, or nullptr; under g_mu. */
sealer_t *sealer_of(const catalog_t *cat) {
  auto it = g_sealers.find(cat);
  return it == g_sealers.end() ? nullptr : it->second;
}

/* Queue a job of table oid on z, once; under z->mu. */
void enqueue_locked(sealer_t *z, uint64_t oid, uint64_t fsid = 0,
                    int kind = job_seal) {
  job_t j{oid, fsid, kind};
  if (z->queued.insert(j).second) {
    z->queue.push_back(j);
    z->cv.notify_one();
  }
}

/*
 * Seal table oid until its delta store has too few rows, each row
 * group in a transaction of its own:
 * 1. Begin at READ COMMITTED: the seal takes no part in SSI.
 * 2. Open the table to write, and seal one row group.
 * 3. Commit; a retryable failure tries the row group again, once.
 * A table dropped since ends the job.
 */
void seal_table(sealer_t *z, session_t *ss, uint64_t oid) {
  int retries = 1;
  for (;;) {
    if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
      return;
    }
    table_t *t = nullptr;
    bool sealed = false;
    int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
    if (rc == 0) {
      rc = tbl_seal(t, sealed);
      tbl_close(t);
    }
    if (rc == 0 && commit(ss) == 0) {
      if (!sealed) {
        return;
      }
      continue;
    }
    bool again = sm1::retryable() == 1 && retries-- > 0;
    if (sm1::errnum() != sm1::SM_ERR_NOTFOUND && !again) {
      sm0::log_msg(sm1::loglevel_t::warn,
                   "column store: the seal of table {} failed: {}", oid,
                   sm1::errmsg());
    }
    abort(ss);
    if (!again) {
      return;
    }
  }
}

/*
 * The live rows of the row group whose RG key value is value, of file
 * set fsid, into live, and its rows into rows: its rows less those
 * its validmap and its delete markers name.
 */
int live_rows(table_t *t, uint64_t fsid, std::string_view value, uint64_t &live,
              uint64_t &rows) {
  rg_meta_t meta;
  std::vector<bool> dead;
  uint32_t ndead = 0, markers = 0;
  if (rg_meta_decode(value, meta) != 0 ||
      col_dead(t, fsid, meta.rows, dead, ndead, markers) != 0) {
    return -1;
  }
  rows = meta.rows;
  live = rows - std::min<uint64_t>(ndead, rows);
  return 0;
}

/* What a compaction's job does with a row group's version. */
enum rg_plan_t : int { plan_none = 0, plan_compact = 1, plan_eliminate = 2 };

/*
 * What to do with file set fsid of table t, a row group's version,
 * into plan (../../COLSTORE.md §11.2, §11.3):
 * 1. Gone, as a job before took it: nothing.
 * 2. Under half a row group's rows live: eliminate it, so its rows
 *    are sealed again with others.
 * 3. A fifth dead: compact it, or eliminate it at version 3.
 * 4. Else nothing.
 */
int rg_plan(table_t *t, uint64_t fsid, int &plan) {
  plan = plan_none;
  // 1. The version.
  std::vector<std::pair<uint64_t, std::string>> rgs;
  if (sm1::fileset_list(tbl_session(t)->sm1, tbl_def(t).storeid, rgs) != 0) {
    return -1;
  }
  auto at = std::find_if(rgs.begin(), rgs.end(),
                         [&](const auto &r) { return r.first == fsid; });
  if (at == rgs.end()) {
    return 0;
  }
  uint64_t live = 0, rows = 0;
  if (live_rows(t, fsid, at->second, live, rows) != 0) {
    return -1;
  }
  // 2. Small; 3. a fifth dead.
  if (live < rg_rows(tbl_session(t)->cat) / 2) {
    plan = plan_eliminate;
  } else if ((rows - live) * 5 >= rows) {
    plan = fsid_version(fsid) < rg_last_version ? plan_compact : plan_eliminate;
  }
  return 0;
}

/*
 * Compact or eliminate file set fsid of table oid, as rg_plan says, in
 * a READ COMMITTED transaction of its own. A writer in flight makes it
 * give up: the next delete markers committed on the row group queue
 * it again. The version done leaves its markers and validmap to the
 * snapshots that see it; they go after, by a cleanup.
 */
void compact_rg(sealer_t *z, session_t *ss, uint64_t oid, uint64_t fsid) {
  if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
    return;
  }
  table_t *t = nullptr;
  bool done = false;
  int plan = plan_none;
  int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
  if (rc == 0) {
    rc = rg_plan(t, fsid, plan);
    if (rc == 0 && plan == plan_compact) {
      rc = tbl_compact_rg(t, fsid, done);
    } else if (rc == 0 && plan == plan_eliminate) {
      rc = tbl_eliminate_rg(t, fsid, done);
    }
    tbl_close(t);
  }
  if (rc == 0 && commit(ss) == 0) {
    if (done) {
      std::lock_guard<std::mutex> g(z->mu);
      z->cleanups.insert({oid, fsid});
    }
    return;
  }
  // A row group gone since, by a job before, ends the job.
  if (sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
    sm0::log_msg(sm1::loglevel_t::warn,
                 "column store: the compaction of file set {} of table "
                 "{} failed: {}",
                 fsid, oid, sm1::errmsg());
  }
  abort(ss);
}

/*
 * Fold file set fsid of table oid (../../COLSTORE.md §11.1), in a
 * READ COMMITTED transaction of its own. A writer in flight makes it
 * give up: the next delete markers committed on it, or a scan, queue
 * it again.
 */
void fold_rg(session_t *ss, uint64_t oid, uint64_t fsid) {
  if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
    return;
  }
  table_t *t = nullptr;
  bool done = false;
  int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
  if (rc == 0) {
    rc = tbl_fold(t, fsid, done);
    tbl_close(t);
  }
  if (rc == 0 && commit(ss) == 0) {
    return;
  }
  // A table gone since ends the job.
  if (sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
    sm0::log_msg(sm1::loglevel_t::warn,
                 "column store: the fold of file set {} of table {} "
                 "failed: {}",
                 fsid, oid, sm1::errmsg());
  }
  abort(ss);
}

/*
 * Clean up old version fsid of table oid (../../COLSTORE.md §11.4), in
 * a READ COMMITTED transaction of its own: true when it is done, or
 * the table is gone; false to try it again.
 */
bool cleanup_rg(session_t *ss, uint64_t oid, uint64_t fsid) {
  if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
    return false;
  }
  table_t *t = nullptr;
  bool done = false;
  int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
  if (rc == 0) {
    rc = tbl_cleanup(t, fsid, done);
    tbl_close(t);
  }
  if (rc == 0 && commit(ss) == 0) {
    return done;
  }
  bool gone = sm1::errnum() == sm1::SM_ERR_NOTFOUND;
  abort(ss);
  return gone;
}

/* One pass over z's cleanups, in a session of its own; not under z->mu. */
void cleanup_pass(sealer_t *z,
                  const std::set<std::pair<uint64_t, uint64_t>> &todo) {
  sm1::session_t *s = z->open();
  session_t *ss = nullptr;
  std::set<std::pair<uint64_t, uint64_t>> finished;
  if (s && session_open(z->cat, s, ss) == 0) {
    for (const auto &[oid, fsid] : todo) {
      if (cleanup_rg(ss, oid, fsid)) {
        finished.insert({oid, fsid});
      }
    }
    session_close(ss);
  }
  if (s) {
    sm1::closedb(s);
  }
  std::lock_guard<std::mutex> g(z->mu);
  for (const auto &c : finished) {
    z->cleanups.erase(c);
  }
}

/*
 * The first pass, at the sealer's start (../../COLSTORE.md §11.4): a
 * restart lost the cleanups' hints, so each column store table's old
 * versions with markers or a validmap left are found again.
 * 1. List the column store tables of the catalog.
 * 2. For each, in a READ COMMITTED transaction, find its old versions.
 * 3. Give each a cleanup hint.
 */
void restart_pass(sealer_t *z) {
  // 1. The column store tables.
  std::vector<uint64_t> oids;
  {
    std::shared_lock<std::shared_mutex> l(z->cat->mu);
    for (const auto &[oid, def] : z->cat->tables) {
      if (def->column_store) {
        oids.push_back(oid);
      }
    }
  }
  if (oids.empty()) {
    return;
  }
  // 2. Their old versions, in a session of the sealer's own.
  std::set<std::pair<uint64_t, uint64_t>> found;
  sm1::session_t *s = z->open();
  session_t *ss = nullptr;
  if (s && session_open(z->cat, s, ss) == 0) {
    for (uint64_t oid : oids) {
      if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
        break;
      }
      table_t *t = nullptr;
      std::set<uint64_t> fsids;
      int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t);
      if (rc == 0) {
        rc = col_orphans(t, fsids);
        tbl_close(t);
      }
      if (rc == 0 && commit(ss) == 0) {
        for (uint64_t fsid : fsids) {
          found.insert({oid, fsid});
        }
        continue;
      }
      // A table dropped since is passed over.
      abort(ss);
    }
    session_close(ss);
  }
  if (s) {
    sm1::closedb(s);
  }
  // 3. The hints.
  std::lock_guard<std::mutex> g(z->mu);
  z->cleanups.insert(found.begin(), found.end());
}

/* The worker: take a job from the queue, and do it. */
void work(sealer_t *z) {
  // ty on this thread allocates from an arena of the thread, which the
  // loop resets after each job, so that it does not grow without end.
  sm0::arena_t arena;
  static thread_local sm0::arena_t *t_arena;
  t_arena = &arena;
  // ty keeps its functions per thread, so each thread sets its own.
  ty_set_alloc(
      [](size_t n) -> void * { return t_arena->alloc((n + 7) & ~(size_t)7); },
      [](void *) {});
  // The cleanups a restart lost, found again first.
  restart_pass(z);
  arena.reset();
  std::unique_lock<std::mutex> g(z->mu);
  for (;;) {
    // With cleanups to try, it wakes each second, as the reapers do.
    if (z->cleanups.empty()) {
      z->cv.wait(g, [&] { return z->stop || !z->queue.empty(); });
    } else {
      z->cv.wait_for(g, std::chrono::seconds(1),
                     [&] { return z->stop || !z->queue.empty(); });
    }
    if (z->stop) {
      return;
    }
    if (z->queue.empty()) {
      std::set<std::pair<uint64_t, uint64_t>> todo = z->cleanups;
      z->busy = true;
      g.unlock();
      cleanup_pass(z, todo);
      arena.reset();
      g.lock();
      z->busy = false;
      z->cv.notify_all();
      continue;
    }
    job_t job = z->queue.front();
    auto [oid, rg_id, kind] = job;
    z->queue.pop_front();
    z->queued.erase(job);
    z->busy = true;
    g.unlock();
    // A session of its own on the database, for this job.
    sm1::session_t *s = z->open();
    session_t *ss = nullptr;
    if (s && session_open(z->cat, s, ss) == 0) {
      if (job_seal == kind) {
        seal_table(z, ss, oid);
      } else if (job_compact == kind) {
        compact_rg(z, ss, oid, rg_id);
      } else {
        fold_rg(ss, oid, rg_id);
      }
      session_close(ss);
    }
    if (s) {
      sm1::closedb(s);
    }
    // The job is done: nothing of ty's from it is live.
    arena.reset();
    g.lock();
    z->busy = false;
    z->cv.notify_all();
  }
}

} // namespace

/* Start cat's sealer; see colstore.h. */
int sealer_attach(catalog_t *cat, std::function<sm1::session_t *()> open) {
  std::lock_guard<std::mutex> g(g_mu);
  if (sealer_of(cat)) {
    return 0;
  }
  sealer_t *z = new sealer_t;
  z->cat = cat;
  z->open = std::move(open);
  // The worker, which waits for a table to seal.
  z->thread = std::thread(work, z);
  g_sealers[cat] = z;
  return 0;
}

/* Stop and join cat's sealer; see colstore.h. */
void sealer_detach(catalog_t *cat) {
  sealer_t *z = nullptr;
  {
    std::lock_guard<std::mutex> g(g_mu);
    z = sealer_of(cat);
    g_sealers.erase(cat);
  }
  if (!z) {
    return;
  }
  {
    std::lock_guard<std::mutex> g(z->mu);
    z->stop = true;
    z->cv.notify_all();
  }
  // The job under way ends its transaction first.
  z->thread.join();
  delete z;
}

/* Count rows put into a delta store; see colstore.h. */
void sealer_count(const catalog_t *cat, uint64_t oid, uint64_t rows) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (!z) {
    return;
  }
  std::lock_guard<std::mutex> h(z->mu);
  uint64_t &n = z->counts[oid];
  n += rows;
  if (n >= rg_rows(cat)) {
    n = 0;
    enqueue_locked(z, oid);
  }
}

/* Count delete markers committed on a file set; see colstore.h. */
void sealer_count_marks(const catalog_t *cat, uint64_t oid, uint64_t fsid,
                        uint64_t marks) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (!z) {
    return;
  }
  std::lock_guard<std::mutex> h(z->mu);
  uint64_t &n = z->marks[{oid, fsid}];
  uint64_t &f = z->fold_marks[{oid, fsid}];
  n += marks;
  f += marks;
  // A fifth of a row group's rows (../COLSTORE.md §11.1): compact it.
  if (n >= rg_rows(cat) / 5) {
    z->marks.erase({oid, fsid});
    z->fold_marks.erase({oid, fsid});
    enqueue_locked(z, oid, fsid, job_compact);
    return;
  }
  // 1% of its rows (../COLSTORE.md §11.1): fold its markers.
  if (f >= std::max<uint64_t>(1, rg_rows(cat) / 100)) {
    z->fold_marks.erase({oid, fsid});
    enqueue_locked(z, oid, fsid, job_fold);
  }
}

/* Queue a row group for its compaction; see colstore.h. */
void sealer_enqueue_rg(const catalog_t *cat, uint64_t oid, uint64_t fsid) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (z) {
    std::lock_guard<std::mutex> h(z->mu);
    enqueue_locked(z, oid, fsid, job_compact);
  }
}

/* Queue a file set for its fold; see colstore.h. */
void sealer_enqueue_fold(const catalog_t *cat, uint64_t oid, uint64_t fsid) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (z) {
    std::lock_guard<std::mutex> h(z->mu);
    enqueue_locked(z, oid, fsid, job_fold);
  }
}

/* Queue a table for its seal; see colstore.h. */
void sealer_enqueue(const catalog_t *cat, uint64_t oid) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (z) {
    std::lock_guard<std::mutex> h(z->mu);
    enqueue_locked(z, oid);
  }
}

/* Wait until cat's sealer has nothing to do; see colstore.h. */
void sealer_wait_idle(const catalog_t *cat) {
  sealer_t *z = nullptr;
  {
    std::lock_guard<std::mutex> g(g_mu);
    z = sealer_of(cat);
  }
  if (!z) {
    return;
  }
  std::unique_lock<std::mutex> h(z->mu);
  z->cv.wait(h, [&] { return z->queue.empty() && !z->busy; });
}

} // namespace sm3
