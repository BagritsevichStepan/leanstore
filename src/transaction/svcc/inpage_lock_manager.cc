#include "transaction/svcc/inpage_lock_manager.h"
#include "common/exceptions.h"
#include "leanstore/leanstore.h"

namespace leanstore::transaction::svcc {

thread_local InPageLockManager::ReadSet InPageLockManager::read_set;
thread_local InPageLockManager::WriteSet InPageLockManager::write_set;

auto InPageLockManager::EmptyLocalSet() -> bool {
  return read_set.empty() && write_set.empty();
}

void InPageLockManager::ReleaseAllLocks(const UndoCallback &undo_cb, const ReleaseLockCallback &release_lock_cb) {
  for (auto &[tuple, leaf] : read_set) {
    release_lock_cb(tuple, leaf, false);
  }

  // The undo (abort) runs while the key is still in the write set: it re-locks its own key
  for (auto &[tuple, entry] : write_set) {
    undo_cb(tuple, std::span<const u8>(entry.undo_payload));
    release_lock_cb(tuple, entry.leaf, true);
  }

  auto release_tuples = [](auto &set) {
    for (auto &entry : set) {
      LockableTuple::Release(entry.first);
    }
    set.clear();
    if (set.bucket_count() > MAX_IDLE_BUCKETS) {
      set.rehash(0);
    }
  };

  release_tuples(read_set);
  release_tuples(write_set);
}

auto InPageLockManager::TryLockShared(const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id)
  -> bool {
  if (read_set.contains(key) || write_set.contains(key)) {
    return true;
  }

  if (!page_lock_table.TryAcquire(key->KeyView(), LeanStore::worker_thread_id, false)) {
    return false;
  }

  read_set.emplace(LockableTuple::Constructor(*key), page_id);
  return true;
}

auto InPageLockManager::TryLock(std::span<u8> undo_payload, const LockableTuple *key, PageLockTable &page_lock_table,
                                pageid_t page_id) -> bool {
  if (write_set.contains(key)) {
    return true;
  }

  // Upgrade if we already hold a shared lock on this key
  auto shared_lock_it = read_set.find(key);
  const bool holds_shared = (shared_lock_it != read_set.end());
  if (!page_lock_table.TryAcquire(key->KeyView(), LeanStore::worker_thread_id, true, holds_shared)) {
    return false;
  }

  LockableTuple *own_tuple;
  if (holds_shared) {
    own_tuple = shared_lock_it->first;
    read_set.erase(shared_lock_it);
  } else {
    own_tuple = LockableTuple::Constructor(*key);
  }

  write_set.emplace(own_tuple, WriteEntry{page_id, std::vector<u8>(undo_payload.begin(), undo_payload.end())});
  return true;
}

}  // namespace leanstore::transaction::svcc
