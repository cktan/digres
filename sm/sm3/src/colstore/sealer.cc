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
 * committed on it have made a fifth dead (../../COLSTORE.md §11.1).
 */

#include "colstore.h"

#include "sm0_log.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace sm3 {

namespace {

/* A catalog's sealer: its queue, its counts, and its thread. */
struct sealer_t {
  catalog_t *cat = nullptr;
  std::function<sm1::session_t *()> open;
  std::mutex mu;
  std::condition_variable cv;
  /* Jobs, in order: (table, 0) a seal, (table, rg_id) a compaction. */
  std::deque<std::pair<uint64_t, uint64_t>> queue;
  std::set<std::pair<uint64_t, uint64_t>> queued; /* the jobs in queue */
  /* Delete markers committed on a row group, by (table, rg_id). */
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> marks;
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

/* Queue table oid on z, once; under z->mu. */
void enqueue_locked(sealer_t *z, uint64_t oid, uint64_t rg_id = 0) {
  if (z->queued.insert({oid, rg_id}).second) {
    z->queue.push_back({oid, rg_id});
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
 * Compact row group rg_id of table oid, in a READ COMMITTED
 * transaction of its own. A writer in flight makes it give up: the
 * next delete markers committed on the row group queue it again.
 */
void compact_rg(session_t *ss, uint64_t oid, uint64_t rg_id) {
  if (begin(ss, sm1::xctlevel_t::read_committed) != 0) {
    return;
  }
  table_t *t = nullptr;
  bool done = false;
  uint64_t rgs[1] = {rg_id};
  int rc = tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t);
  if (rc == 0) {
    rc = tbl_compact_rgs(t, rgs, done);
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

/* The worker: take a job from the queue, and do it. */
void work(sealer_t *z) {
  std::unique_lock<std::mutex> g(z->mu);
  for (;;) {
    z->cv.wait(g, [&] { return z->stop || !z->queue.empty(); });
    if (z->stop) {
      return;
    }
    auto [oid, rg_id] = z->queue.front();
    z->queue.pop_front();
    z->queued.erase({oid, rg_id});
    z->busy = true;
    g.unlock();
    // A session of its own on the database, for this job.
    sm1::session_t *s = z->open();
    session_t *ss = nullptr;
    if (s && session_open(z->cat, s, ss) == 0) {
      if (rg_id == 0) {
        seal_table(z, ss, oid);
      } else {
        compact_rg(ss, oid, rg_id);
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
  if (n >= rg_max_rows) {
    n = 0;
    enqueue_locked(z, oid);
  }
}

/* Count delete markers committed on a row group; see colstore.h. */
void sealer_count_marks(const catalog_t *cat, uint64_t oid, uint64_t rg_id,
                        uint64_t marks) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (!z) {
    return;
  }
  std::lock_guard<std::mutex> h(z->mu);
  uint64_t &n = z->marks[{oid, rg_id}];
  n += marks;
  // A fifth of a row group's rows (../COLSTORE.md §11.1).
  if (n >= rg_max_rows / 5) {
    z->marks.erase({oid, rg_id});
    enqueue_locked(z, oid, rg_id);
  }
}

/* Queue a row group for its compaction; see colstore.h. */
void sealer_enqueue_rg(const catalog_t *cat, uint64_t oid, uint64_t rg_id) {
  std::lock_guard<std::mutex> g(g_mu);
  sealer_t *z = sealer_of(cat);
  if (z) {
    std::lock_guard<std::mutex> h(z->mu);
    enqueue_locked(z, oid, rg_id);
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
