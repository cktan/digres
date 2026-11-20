/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * fileset.cc -- file sets (../COLSTORE.md §4.5, §13): groups of
 * immutable files, named by a versioned key of class 0x05 in a
 * store. sm1 writes the files and makes them durable, puts and
 * erases the key in a transaction, unlinks the files of an abort,
 * deletes them in GC once no snapshot can see them, and sweeps
 * them when the database opens.
 *
 * A file set's files are under the store's RocksDB directory:
 *
 *   colstore/<storeid>/<fsid / 1024>/<fsid>.<name>
 */

#include "error.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"
#include "version.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>

using namespace sm1_internal;

namespace fs = std::filesystem;

namespace {

/* File sets to a directory (../COLSTORE.md §4.1). */
constexpr uint64_t per_dir = 1024;

/* The userkey of a file set: its fsid, big-endian. */
std::string fs_userkey(uint64_t fsid) {
  std::string k;
  sm0::put_be64(k, fsid);
  return k;
}

/* The directory of store storeid's file sets, in the instance rdb. */
fs::path store_dir(const rocksdb::DB *rdb, uint64_t storeid) {
  return fs::path(rdb->GetName()) / "colstore" / std::to_string(storeid);
}

/* The directory of file set fsid. */
fs::path set_dir(const rocksdb::DB *rdb, uint64_t storeid, uint64_t fsid) {
  return store_dir(rdb, storeid) / std::to_string(fsid / per_dir);
}

/* A name of a file set's file: letters, digits, '.' and '_'. */
int check_name(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return set_error(sm1::SM_ERR_INVALID, "file set file name of {} bytes",
                     name.size());
  }
  for (char c : name) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') {
      return set_error(sm1::SM_ERR_INVALID, "bad file set file name");
    }
  }
  return 0;
}

/* Write all of data to fd at its offset; false on a failure. */
bool write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

/* Make dir durable: fsync it, so a file made in it stays. */
int sync_dir(const fs::path &dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return set_errno_error(errno, "open a file set directory");
  }
  int r = ::fsync(fd);
  int e = errno;
  ::close(fd);
  return r == 0 ? 0 : set_errno_error(e, "fsync a file set directory");
}

} // namespace

namespace sm1_internal {

/* Unlink every file of file set fsid; see session.h. */
void fileset_unlink(const rocksdb::DB *rdb, uint64_t storeid, uint64_t fsid) {
  std::error_code ec;
  fs::path dir = set_dir(rdb, storeid, fsid);
  std::string prefix = std::to_string(fsid) + ".";
  for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.compare(0, prefix.size(), prefix) == 0) {
      fs::remove(e.path(), ec);
    }
  }
}

/* Remove the directory of a store's file sets; see session.h. */
void fileset_drop_store(const rocksdb::DB *rdb, uint64_t storeid) {
  std::error_code ec;
  fs::remove_all(store_dir(rdb, storeid), ec);
}

/* Unlink the files the transaction wrote after the first n; see session.h. */
void fileset_discard(sm1::session_t *s, size_t n) {
  for (size_t i = n; i < s->fs_files.size(); i++) {
    std::error_code ec;
    fs::remove(s->fs_files[i], ec);
  }
  s->fs_files.resize(std::min(n, s->fs_files.size()));
}

/*
 * Sweep the file sets of every store of db (../COLSTORE.md §13):
 * delete the files of an fsid none of whose versions is committed
 * and live, or prepared, and the directories of stores that are
 * gone. Run at open, after recovery, before any session.
 */
void fileset_sweep(db_t *db) {
  std::error_code ec;
  fs::path root = fs::path(db->rdb->GetName()) / "colstore";
  if (!fs::exists(root, ec)) {
    return;
  }
  for (const fs::directory_entry &sd : fs::directory_iterator(root, ec)) {
    uint64_t storeid = strtoull(sd.path().filename().c_str(), nullptr, 10);
    store_t st = db_store(db, storeid);
    // A store that is gone: its directory goes with it.
    if (!st.cf) {
      fs::remove_all(sd.path(), ec);
      continue;
    }
    // The fsids whose files stay.
    std::unordered_set<uint64_t> live;
    rocksdb::ReadOptions ro;
    ro.total_order_seek = true;
    std::string lo(1, key_class_fileset), hi(1, key_class_fileset + 1);
    rocksdb::Slice upper(hi);
    ro.iterate_upper_bound = &upper;
    std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
    // The newest committed or prepared version of each fsid decides.
    std::string decided;
    for (it->Seek(lo); it->Valid(); it->Next()) {
      version_t v;
      version_decode_key(it->key(), &v);
      if (v.userkey.size() != 8 || !version_decode_value(it->value(), &v)) {
        continue;
      }
      std::string uk(v.userkey.data(), v.userkey.size());
      if (uk == decided) {
        continue;
      }
      uint64_t ts;
      xct_state_t state = (v.flags & version_stamped)
                              ? XCT_COMMITTED
                              : xct_status(db, v.xid, &ts);
      // A prepared transaction's: kept, its fate still to come.
      if (state == XCT_IN_FLIGHT || state == XCT_ERROR) {
        live.insert(sm0::get_be64(uk.data()));
        decided = uk;
      } else if (state == XCT_COMMITTED) {
        if (!(v.flags & version_tombstone)) {
          live.insert(sm0::get_be64(uk.data()));
        }
        decided = uk;
      }
    }
    // Every file of an fsid not live goes.
    for (const fs::directory_entry &dd :
         fs::directory_iterator(sd.path(), ec)) {
      for (const fs::directory_entry &f :
           fs::directory_iterator(dd.path(), ec)) {
        uint64_t fsid = strtoull(f.path().filename().c_str(), nullptr, 10);
        if (!live.count(fsid)) {
          fs::remove(f.path(), ec);
        }
      }
    }
  }
}

} // namespace sm1_internal

/* Write one file of a file set, durably; see sm1.h. */
int sm1::fileset_write(sm1::session_t *s, uint64_t storeid, uint64_t fsid,
                       std::string_view name, std::string_view bytes) {
  if (xct_check_active(s) != 0 || check_name(name) != 0) {
    return -1;
  }
  const store_t &st = session_store(s, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  fs::path dir = set_dir(st.rdb, storeid, fsid);
  std::error_code ec;
  // Make the directories, and keep them: a new one is synced below.
  bool made = fs::create_directories(dir, ec);
  if (ec) {
    return set_errno_error(ec.value(), "make a file set directory");
  }
  fs::path path = dir / (std::to_string(fsid) + "." + std::string(name));
  // A file is written once: an existing one is never replaced.
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    return set_errno_error(errno, "create a file set file");
  }
  // Listed now, so an abort unlinks it whatever happens next.
  s->fs_files.push_back(path.string());
  bool ok = write_all(fd, bytes.data(), bytes.size()) && ::fdatasync(fd) == 0;
  int e = errno;
  ::close(fd);
  if (!ok) {
    return set_errno_error(e, "write a file set file");
  }
  // The file's name, and a new directory's, must survive a crash.
  if (sync_dir(dir) != 0 || (made && sync_dir(dir.parent_path()) != 0) ||
      (made && sync_dir(dir.parent_path().parent_path()) != 0)) {
    return -1;
  }
  return 0;
}

/* Read a range of one file of a file set; see sm1.h. */
int sm1::fileset_read(sm1::session_t *s, uint64_t storeid, uint64_t fsid,
                      std::string_view name, uint64_t offset, uint64_t length,
                      std::string &out) {
  out.clear();
  if (xct_check_active(s) != 0 || check_name(name) != 0) {
    return -1;
  }
  const store_t &st = session_store(s, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  fs::path path = set_dir(st.rdb, storeid, fsid) /
                  (std::to_string(fsid) + "." + std::string(name));
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    // A file a visible key names is missing: corruption.
    return errno == ENOENT
               ? set_error(sm1::SM_ERR_CORRUPT, "file set file {} is missing",
                           path.string())
               : set_errno_error(errno, "open a file set file");
  }
  defer { ::close(fd); };
  if (length == UINT64_MAX) {
    off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0 || static_cast<uint64_t>(end) < offset) {
      return set_error(sm1::SM_ERR_CORRUPT, "file set file {} is short",
                       path.string());
    }
    length = static_cast<uint64_t>(end) - offset;
  }
  out.resize(length);
  size_t got = 0;
  while (got < length) {
    ssize_t r = ::pread(fd, out.data() + got, length - got,
                        static_cast<off_t>(offset + got));
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0) {
      out.clear();
      return set_errno_error(errno, "read a file set file");
    }
    if (r == 0) {
      out.clear();
      return set_error(sm1::SM_ERR_CORRUPT, "file set file {} is short",
                       path.string());
    }
    got += static_cast<size_t>(r);
  }
  return 0;
}

namespace {

/*
 * Put a version of file set fsid: its payload, or a tombstone with
 * erase (../COLSTORE.md §4.5). A put is of an fsid never used, so
 * it reads nothing. An erase waits for a writer in flight, and
 * judges first-committer-wins as any write does above read
 * committed. Returns 0 or -1.
 */
int fs_write(sm1::session_t *s, uint64_t storeid, uint64_t fsid,
             std::string_view payload, bool erase) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  const store_t &st = session_store(s, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  if (!st.owner && xct_check_readonly(s) != 0) {
    return -1;
  }
  std::string uk = fs_userkey(fsid);
  // The latch names the key with its class: a row's userkey may
  // have the same bytes.
  std::string latch_key = std::string(1, key_class_fileset) + uk;
  std::unique_lock<std::mutex> latch(lock_latch(s->db, storeid, latch_key));
  if (erase) {
    // The newest version not dead decides: wait for one in flight.
    for (;;) {
      uint64_t inflight = 0, commit_ts = 0;
      bool live = false, own = false;
      {
        uint64_t epoch = db_read_enter(s->db);
        defer { db_read_leave(s->db, epoch); };
        rocksdb::ReadOptions ro;
        ro.prefix_same_as_start = true;
        std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
        for (it->Seek(version_bound(uk, '\x00', key_class_fileset));
             it->Valid(); it->Next()) {
          version_t v;
          version_decode_key(it->key(), &v);
          if (it->key()[0] != key_class_fileset || v.userkey != uk) {
            break;
          }
          if (!version_decode_value(it->value(), &v)) {
            return set_error(sm1::SM_ERR_CORRUPT, "malformed version value");
          }
          if (s->xid != 0 && v.xid == s->xid && !(v.flags & version_stamped)) {
            if (v.seq > s->seq_hwm) {
              continue;
            }
            own = true;
            live = !(v.flags & version_tombstone);
            break;
          }
          uint64_t ts = v.commit_ts;
          xct_state_t state = XCT_COMMITTED;
          if (!(v.flags & version_stamped)) {
            state = xct_status(s->db, v.xid, &ts);
          }
          if (state == XCT_ERROR) {
            return -1;
          }
          if (state == XCT_IN_FLIGHT) {
            inflight = v.xid;
            break;
          }
          if (state == XCT_COMMITTED) {
            live = !(v.flags & version_tombstone);
            commit_ts = ts;
            break;
          }
        }
      }
      if (inflight != 0) {
        latch.unlock();
        int waited = lock_wait_xact(s, inflight);
        if (waited != 0) {
          return lock_wait_error(s, waited);
        }
        latch.lock();
        continue;
      }
      if (!own && commit_ts > s->snapshot_ts &&
          s->level != sm1::xctlevel_t::read_committed) {
        latch.unlock();
        return xct_fail_retryable(s, sm1::SM_ERR_CONFLICT,
                                  "file set was changed by a transaction "
                                  "that committed after this one began");
      }
      if (!live) {
        return set_error(sm1::SM_ERR_NOTFOUND, "file set {} does not exist",
                         fsid);
      }
      break;
    }
  }
  if (s->seq == UINT32_MAX) {
    return set_error(sm1::SM_ERR_INVALID, "too many writes in one transaction");
  }
  // The xid at the first write.
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  uint32_t seq = s->seq + 1;
  s->write_gen++;
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  std::string vkey = version_key(uk, wts, seq, key_class_fileset);
  std::string vval = version_value(erase ? version_tombstone : 0, s->xid,
                                   erase ? rocksdb::Slice() : payload);
  rocksdb::Status status =
      st.rdb->Put(store_write_options(st), st.cf, vkey, vval);
  if (!status.ok()) {
    return set_status_error(status, "write a file set key");
  }
  count_write(st, uk, vkey.size() + vval.size());
  s->seq = seq;
  s->seq_hwm = seq;
  s->wts_hi = wts;
  // A rollback to a savepoint erases it again.
  sm1::session_t::undo_t u{storeid, uk, wts, seq};
  u.cls = key_class_fileset;
  access_undo_add(s, std::move(u));
  if (st.owner) {
    s->temp_touched = true;
  }
  return 0;
}

} // namespace

/* Put a file set's key, naming its files; see sm1.h. */
int sm1::fileset_put(sm1::session_t *s, uint64_t storeid, uint64_t fsid,
                     std::string_view payload) {
  if (payload.size() > sm1::max_datalen) {
    return set_error(sm1::SM_ERR_INVALID, "value of {} bytes exceeds {}",
                     payload.size(), sm1::max_datalen);
  }
  return fs_write(s, storeid, fsid, payload, false);
}

/* Erase a file set's key; see sm1.h. */
int sm1::fileset_erase(sm1::session_t *s, uint64_t storeid, uint64_t fsid) {
  return fs_write(s, storeid, fsid, {}, true);
}

/* The file sets of a store the transaction sees; see sm1.h. */
int sm1::fileset_list(sm1::session_t *s, uint64_t storeid,
                      std::vector<std::pair<uint64_t, std::string>> &out) {
  out.clear();
  if (xct_check_active(s) != 0) {
    return -1;
  }
  const store_t &st = session_store(s, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  view_t view = xct_view(s);
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::string lo(1, key_class_fileset), hi(1, key_class_fileset + 1);
  rocksdb::Slice upper(hi);
  ro.iterate_upper_bound = &upper;
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
  // The first visible version of each fsid decides it.
  std::string decided;
  for (it->Seek(lo); it->Valid(); it->Next()) {
    version_t v;
    version_decode_key(it->key(), &v);
    if (v.userkey.size() != 8) {
      continue;
    }
    std::string uk(v.userkey.data(), v.userkey.size());
    if (uk == decided) {
      continue;
    }
    if (!version_decode_value(it->value(), &v)) {
      return set_error(sm1::SM_ERR_CORRUPT, "malformed version value");
    }
    int r = access_visible(s->db, v, view, nullptr, nullptr);
    if (r < 0) {
      return -1;
    }
    if (r == 1) {
      decided = uk;
      if (!(v.flags & version_tombstone)) {
        out.emplace_back(sm0::get_be64(uk.data()),
                         std::string(v.payload.data(), v.payload.size()));
      }
    }
  }
  if (!it->status().ok()) {
    return set_status_error(it->status(), "read file set keys");
  }
  return 0;
}
