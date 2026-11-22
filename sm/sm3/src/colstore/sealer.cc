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
 * lost. The same worker compacts a row group that delete markers
 * committed on it have made a fifth dead, and merges the small row
 * groups next to it with it (../../COLSTORE.md §11.1).
 */

#include "colstore.h"

#include "sm0_log.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
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

/*
 * The row groups of table t to compact with rg_id, into out
 * (../../COLSTORE.md §11.1): rg_id, and, if it is small, with under
 * half a row group's rows live, the small row groups next to it in
 * rg_id order, while the live rows of all fit one row group. None if
 * there is nothing to gain, or rg_id is gone.
 * 1. List the row groups, in rg_id order.
 * 2. Count rg_id's live rows.
 * 3. If it is small, take each small row group after it, then each
 *    before it, while they fit.
 * 4. Alone, and less than a fifth dead, it is left as it is.
 */
int merge_set(table_t *t, uint64_t rg_id, std::vector<uint64_t> &out) {
  out.clear();
  // 1. The row groups.
  std::vector<std::pair<uint64_t, std::string>> rgs;
  if (sm1::fileset_list(tbl_session(t)->sm1, tbl_def(t).storeid, rgs) != 0) {
    return -1;
  }
  std::sort(rgs.begin(), rgs.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  auto at = std::find_if(rgs.begin(), rgs.end(),
                         [&](const auto &r) { return r.first == rg_id; });
  if (at == rgs.end()) {
    return 0;
  }
  const ptrdiff_t x = at - rgs.begin();
  // 2. Its live rows.
  uint64_t live = 0, rows = 0;
  if (live_rows(t, rg_id, at->second, live, rows) != 0) {
    return -1;
  }
  out.push_back(rg_id);
  // 3. The small row groups next to it.
  const uint64_t max_rows = rg_rows(tbl_session(t)->cat);
  const uint64_t half = max_rows / 2;
  if (live < half) {
    uint64_t total = live;
    for (ptrdiff_t dir : {ptrdiff_t(1), ptrdiff_t(-1)}) {
      for (ptrdiff_t j = x + dir;
           j >= 0 && j < static_cast<ptrdiff_t>(rgs.size()); j += dir) {
        uint64_t lj = 0, rj = 0;
        if (live_rows(t, rgs[j].first, rgs[j].second, lj, rj) != 0) {
          return -1;
        }
        if (lj >= half || total + lj > max_rows) {
          break;
        }
        total += lj;
        out.push_back(rgs[j].first);
      }
    }
  }
  // 4. Nothing to gain.
  if (out.size() == 1 && (rows - live) * 5 < rows) {
    out.clear();
  }
  return 0;
}

/*
 * Compact row group rg_id of table oid, with the small row groups next
 * to it (merge_set), in a READ COMMITTED transaction of its own. A
 * writer in flight makes it give up: the next delete markers committed
 * on the row group queue it again.
 */
void compact_rg(session_t *ss, uint64_t oid, uint64_t rg_id) {
  if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
    return;
  }
  table_t *t = nullptr;
  bool done = false;
  std::vector<uint64_t> rgs;
  int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
  if (rc == 0) {
    rc = merge_set(t, rg_id, rgs);
    if (rc == 0 && !rgs.empty()) {
      rc = tbl_compact_rgs(t, rgs, done);
    }
    tbl_close(t);
  }
  if (rc == 0 && commit(ss) == 0) {
    return;
  }
  // A row group gone since, by a compaction before, ends the job.
  if (sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
    sm0::log_msg(sm1::loglevel_t::warn,
                 "column store: the compaction of row group {} of table "
                 "{} failed: {}",
                 rg_id, oid, sm1::errmsg());
  }
  abort(ss);
}

/*
 * Fold file set fsid of table oid (../../COLSTORE2.md §6.6), in a
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

/* The worker: take a job from the queue, and do it. */
void work(sealer_t *z) {
  std::unique_lock<std::mutex> g(z->mu);
  for (;;) {
    z->cv.wait(g, [&] { return z->stop || !z->queue.empty(); });
    if (z->stop) {
      return;
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
        compact_rg(ss, oid, rg_id);
      } else {
        fold_rg(ss, oid, rg_id);
      }
      session_close(ss);
    }
    if (s) {
      sm1::closedb(s);
    }
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
  // 1% of its rows (../COLSTORE2.md §3): fold its markers.
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
