#include "transaction/svcc/inpage_lock_manager.h"
#include "common/exceptions.h"
#include "leanstore/leanstore.h"

#include <cstring>

namespace leanstore::transaction::svcc {

thread_local InPageLockManager::HeldLocks InPageLockManager::held_locks;
thread_local detail::TxnArena InPageLockManager::arena;

namespace {

auto CopyTuple(detail::TxnArena &arena, const LockableTuple &tuple) -> const LockableTuple * {
  auto copy = reinterpret_cast<LockableTuple *>(arena.Allocate(sizeof(LockableTuple) + tuple.key_len,
                                                               alignof(LockableTuple)));
  copy->tree_id = tuple.tree_id;
  copy->key_len = tuple.key_len;
  std::memcpy(copy->key, tuple.key, tuple.key_len);
  return copy;
}

} // namespace

auto InPageLockManager::TryLockShared(const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id)
  -> bool {
  const auto hash = held_locks.hash(key);
  if (held_locks.find(key, hash) != held_locks.end()) {
    return true;
  }

  if (!page_lock_table.TryAcquire({key->key, key->key_len}, LeanStore::worker_thread_id, false)) {
    return false;
  }

  held_locks.emplace_with_hash(hash, CopyTuple(arena, *key), HeldLock{page_id, false, {}});
  return true;
}

auto InPageLockManager::TryLock(std::span<u8> undo_payload, const LockableTuple *key, PageLockTable &page_lock_table,
                                pageid_t page_id) -> bool {
  const auto hash = held_locks.hash(key);

  auto held_lock = held_locks.find(key, hash);
  if (held_lock != held_locks.end() && held_lock->second.exclusive) {
    return true;
  }

  const bool holds_shared = (held_lock != held_locks.end());
  if (!page_lock_table.TryAcquire({key->key, key->key_len}, LeanStore::worker_thread_id, true, holds_shared)) {
    return false;
  }

  if (holds_shared) {
    held_lock->second = HeldLock{page_id, true, arena.Copy(undo_payload)};
  } else {
    held_locks.emplace_with_hash(hash, CopyTuple(arena, *key), HeldLock{page_id, true, arena.Copy(undo_payload)});
  }

  return true;
}

}  // namespace leanstore::transaction::svcc
