#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "transaction/lockable_tuple.h"
#include "transaction/svcc/detail/txn_arena.h"
#include "transaction/svcc/page_lock_table.h"

#include "parallel_hashmap/phmap.h"

#include <span>

namespace leanstore::transaction::svcc {

/**
 * @brief SVCC 2PL (No-Wait) whose locks live in per-leaf PageLockTables instead of one global lock table.
 */
class InPageLockManager {
 public:
  auto EmptyLocalSet() -> bool;

  template <typename UndoFn, typename ReleaseLockFn>
  void ReleaseAllLocks(UndoFn &&undo_cb, ReleaseLockFn &&release_lock_cb);

  auto TryLockShared(const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id) -> bool;

  auto TryLock(std::span<u8> undo_payload, const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id)
    -> bool;

 private:
  /* Above it, the table of the held locks is freed at the end of a transaction instead of cleared slot by slot */
  static constexpr size_t MAX_IDLE_CAPACITY = 3000;

  struct HeldLock {
    pageid_t leaf;
    bool exclusive;
    std::span<const u8> undo_payload;
  };

  using HeldLocks = phmap::flat_hash_map<const LockableTuple *, HeldLock, LockableTuple::HashPtr,
                                         LockableTuple::EqualPtr>;

  static thread_local HeldLocks held_locks;
  static thread_local detail::TxnArena arena;
};

// Implementation
inline auto InPageLockManager::EmptyLocalSet() -> bool {
  return held_locks.empty();
}

template <typename UndoFn, typename ReleaseLockFn>
void InPageLockManager::ReleaseAllLocks(UndoFn &&undo_cb, ReleaseLockFn &&release_lock_cb) {
  const auto held_lock_count = held_locks.size();
  for (auto &[tuple, lock] : held_locks) {
    if (lock.exclusive) {
      undo_cb(tuple, lock.undo_payload);
    }
  }

  Ensure(held_locks.size() == held_lock_count);

  for (auto &[tuple, lock] : held_locks) {
    release_lock_cb(tuple, lock.leaf, lock.exclusive);
  }

  if (held_locks.capacity() > MAX_IDLE_CAPACITY) {
    HeldLocks().swap(held_locks);
  } else {
    held_locks.clear();
  }

  arena.Reset();
}

}  // namespace leanstore::transaction::svcc
