#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "transaction/lockable_tuple.h"
#include "transaction/svcc/inpage/detail/txn_arena.h"
#include "transaction/svcc/inpage/detail/waiter_workers_slots.h"
#include "transaction/svcc/inpage/page_lock_table.h"

#include "parallel_hashmap/phmap.h"

#include <span>

namespace leanstore::transaction::svcc::inpage {

/**
 * @brief SVCC 2PL (No-Wait) whose locks live in per-leaf PageLockTables instead of one global lock table.
 */
class InPageLockManager {
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
  
 public:
  auto TryLock(LockParams params, std::span<u8> undo_payload, const LockableTuple *key,
               PageLockTable &page_lock_table, pageid_t page_id) -> LockResult;

  template <typename UndoFn, typename ReleaseLockFn>
  void ReleaseAllLocks(timestamp_t txn_ts, UndoFn &&undo_cb, ReleaseLockFn &&release_lock_cb);

  void WaitUntilGranted(wid_t worker_id);

  auto EmptyLocalSet() -> bool;

 private:
  auto TryLockShared(LockParams params, const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id)
    -> LockResult;

  auto TryLockExclusive(LockParams params, std::span<u8> undo_payload, const LockableTuple *key,
                        PageLockTable &page_lock_table, pageid_t page_id) -> LockResult;
  
  void WakeWaiters(std::span<const wid_t> workers);
  
  /* The held locks are stored in a thread-local hash map. */
  static thread_local HeldLocks held_locks;
  static thread_local detail::TxnArena arena;

  /* Used for wake up waiting workers. See Wait-Die algorithm. */
  detail::WaiterWorkersSlots waiter_slots_;
};

// Implementation
inline auto InPageLockManager::TryLock(LockParams params, std::span<u8> undo_payload, const LockableTuple *key,
                                       PageLockTable &page_lock_table, pageid_t page_id) -> LockResult {
  if (params.exclusive) {
    return TryLockExclusive(params, undo_payload, key, page_lock_table, page_id);
  }
  return TryLockShared(params, key, page_lock_table, page_id);
}

template <typename UndoFn, typename ReleaseLockFn>
void InPageLockManager::ReleaseAllLocks(timestamp_t txn_ts, UndoFn &&undo_cb, ReleaseLockFn &&release_lock_cb) {
  const auto held_lock_count = held_locks.size();
  for (auto &[tuple, lock] : held_locks) {
    if (lock.exclusive) {
      undo_cb(tuple, lock.undo_payload);
    }
  }

  Ensure(held_locks.size() == held_lock_count);

  for (auto &[tuple, lock] : held_locks) {
    WakeWaiters(release_lock_cb(txn_ts, tuple, lock.leaf, lock.exclusive));
  }

  if (held_locks.capacity() > MAX_IDLE_CAPACITY) {
    HeldLocks().swap(held_locks);
  } else {
    held_locks.clear();
  }

  arena.Reset();
}

inline void InPageLockManager::WaitUntilGranted(wid_t worker_id) {
  waiter_slots_.Wait(worker_id);
}

inline auto InPageLockManager::EmptyLocalSet() -> bool {
  return held_locks.empty();
}

inline void InPageLockManager::WakeWaiters(std::span<const wid_t> workers) {
  for (auto worker_id : workers) {
    waiter_slots_.WakeWorker(worker_id);
  }
}

}  // namespace leanstore::transaction::svcc::inpage
