/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * version.h -- the row-version codec and the store column
 * families' comparator and prefix extractor (../DBMGR.md §6.2).
 *
 *   key   = class (1B, key_class_versioned) | userkey |
 *           ~wts (8B BE) | ~seq (4B BE)
 *   value = flags (1B) | xid (8B, unstamped) or commit_ts (8B,
 *           stamped) | payload
 *
 * Every key of a store opens with a class byte (../ROWSTORE.md §3.2):
 * versioned keys are ordered by userkey, then newest version
 * first; current and old keys bytewise. No caller of sm1 sees it.
 */
#ifndef SM1_VERSION_H
#define SM1_VERSION_H

#include <stdint.h>

#include <memory>
#include <string>

#include <rocksdb/comparator.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>

namespace sm1_internal {

/* Bytes after the userkey in every version key. */
constexpr size_t version_tail_size = 12;

/* The class byte that opens every key of a store (../ROWSTORE.md §3.2). */
constexpr char key_class_versioned = 0x00; /* userkey | ~wts | ~seq */
constexpr char key_class_current = 0x01;   /* a chained key's newest */
constexpr char key_class_old = 0x02;       /* be64(id): an older one */
constexpr char key_class_page = 0x04;      /* a page, unversioned */
/*
 * A file set's key, versioned as class 0x00's: be64(fsid) | ~wts |
 * ~seq (../COLSTORE.md §4.5). Past the pages, as every key below
 * 0x04 is taken for a row version.
 */
constexpr char key_class_fileset = 0x05;

/* Whether keys of class c end in a version tail: ~wts | ~seq. */
inline bool has_version_tail(char c) {
  return c == key_class_versioned || c == key_class_fileset;
}

/* Value flags. */
constexpr uint8_t version_tombstone = 0x01; /* this version is a delete */
constexpr uint8_t version_stamped =
    0x02; /* commit_ts, not xid, follows flags */
constexpr uint8_t version_moved =
    0x04; /* a tombstone of a row that moved to another key */
/* A chained update that kept its row's key (../LOCKMGR.md §1.13). */
constexpr uint8_t version_key_kept = 0x08;
/*
 * A version that a move of sm's own wrote, which changed no row
 * (../COLSTORE.md §10.2, §11): a sealed tombstone, or a live
 * version, a moved marker. A write and a row lock judge
 * first-committer-wins by the version under it. 0x10 and 0x20 are
 * an OLD entry's flags, and a CURR version keeps its flags when it
 * moves to OLD, so this flag is apart from them.
 */
constexpr uint8_t version_sealed = 0x40;

/* A version key or value, decoded. Slices borrow the input. */
struct version_t {
  rocksdb::Slice userkey;
  uint64_t wts = 0; /* write timestamp, from the key */
  uint32_t seq = 0;
  uint8_t flags = 0;
  uint64_t xid = 0;       /* the writer; valid only if unstamped */
  uint64_t commit_ts = 0; /* valid only if flags & version_stamped */
  rocksdb::Slice payload;
};

/* Build class | userkey | ~wts | ~seq. */
std::string version_key(rocksdb::Slice userkey, uint64_t wts, uint32_t seq,
                        char cls = key_class_versioned);

/* Build class | userkey, then a tail of twelve copies of fill. */
std::string version_bound(rocksdb::Slice userkey, char fill,
                          char cls = key_class_versioned);

/* The userkey part of a version key: past the class, before the tail. */
rocksdb::Slice version_userkey(rocksdb::Slice key);

/* Decode a version key into v's userkey, wts, and seq. */
void version_decode_key(rocksdb::Slice key, version_t *v);

/* Build an unstamped value: flags, xid, then payload. */
std::string version_value(uint8_t flags, uint64_t xid, rocksdb::Slice payload);

/* Decode a version value into v; false if it is malformed. */
bool version_decode_value(rocksdb::Slice value, version_t *v);

/* The sm.store.v2 comparator, a process-wide singleton. */
const rocksdb::Comparator *version_comparator();

/*
 * The prefix extractor: a version key's class and userkey; any other
 * key whole.
 */
std::shared_ptr<const rocksdb::SliceTransform> version_prefix_extractor();

} // namespace sm1_internal

#endif /* SM1_VERSION_H */
