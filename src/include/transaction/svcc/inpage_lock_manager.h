#pragma once

#include "common/typedefs.h"
#include "transaction/lock_manager_interface.h"
#include "transaction/lockable_tuple.h"
#include "transaction/svcc/page_lock_state.h"

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace leanstore::transaction::svcc {

/**
 * @brief SVCC 2PL (No-Wait) whose locks live in per-leaf PageLockTables instead of one global lock table.
 *
 * It does not own the locks: every lock request comes with the PageLockState of the leaf covering the key,
 * and this manager only tracks what the running transaction holds.
 *
 * A transaction remembers the LockState objects it holds and releases them through these pointers,
 * hence commit/abort neither traverses the B-tree nor touches the pages.
 *
 * Readers lock under an optimistic latch on the leaf. If their traversal was stale, the lock lands in a
 * leaf that no longer covers the key, the leaf validation fails and the operation restarts in the right
 * leaf. Such a stale lock is never undone eagerly: no writer looks at it, and it is released on
 * commit/abort like every other lock.
 */
class InPageLockManager : public ILockManager {
 public:
  InPageLockManager()           = default;
  ~InPageLockManager() override = default;

  bool EmptyLocalSet() override;
  void ReleaseAllLocks(timestamp_t txn_ts, const WriteSetCallback &write_set_cb) override;

  bool TryLockShared(timestamp_t txn_ts, const LockableTuple *key, PageLockState *page_lock_state = nullptr) override;
  bool TryLock(timestamp_t txn_ts, timestamp_t undo_ts, std::span<u8> undo_payload, const LockableTuple *key,
               PageLockState *page_lock_state = nullptr) override;
  
  /* Unreachable part. Locks are only released all at once, in ReleaseAllLocks() */
  void Unlock(timestamp_t txn_ts, const LockableTuple *key) override;
  void UnlockShared(timestamp_t txn_ts, const LockableTuple *key) override;

 private:
  struct WriteEntry {
    LockableTuple *tuple;          // heap copy of the key, passed to the write-set callback
    std::vector<u8> undo_payload;  // previous payload, empty if the key did not exist
  };

  // Thread-local set of currently held locks (read/write)
  static thread_local std::unordered_set<LockState *> read_set;
  static thread_local std::unordered_map<LockState *, WriteEntry> write_set;
};

}  // namespace leanstore::transaction::svcc
