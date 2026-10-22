#pragma once

#include "common/typedefs.h"
#include "transaction/lockable_tuple.h"
#include "transaction/svcc/page_lock_table.h"

#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace leanstore::transaction::svcc {

/**
 * @brief SVCC 2PL (No-Wait) whose locks live in per-leaf PageLockTables instead of one global lock table.
 */
class InPageLockManager {
 public:
  /* Undoes a write: re-installs undo_payload, or removes the key if it is empty */
  using UndoCallback = std::function<void(const LockableTuple *, std::span<const u8> undo_payload)>;
  /* Releases the lock of a tuple. leaf_hint is the leaf the lock was acquired on */
  using ReleaseLockCallback = std::function<void(const LockableTuple *, pageid_t leaf_hint, bool exclusive)>;

  auto EmptyLocalSet() -> bool;

  void ReleaseAllLocks(const UndoCallback &undo_cb, const ReleaseLockCallback &release_lock_cb);

  auto TryLockShared(const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id) -> bool;

  auto TryLock(std::span<u8> undo_payload, const LockableTuple *key, PageLockTable &page_lock_table, pageid_t page_id)
    -> bool;

 private:
  struct WriteEntry {
    pageid_t leaf;  // the leaf the lock was acquired on
    std::vector<u8> undo_payload;  // previous payload, empty if the key did not exist
  };

  using ReadSet = std::unordered_map<LockableTuple *, pageid_t, LockableTuple::HashPtr, LockableTuple::EqualPtr>;
  using WriteSet = std::unordered_map<LockableTuple *, WriteEntry, LockableTuple::HashPtr, LockableTuple::EqualPtr>;

  // Thread-local sets of currently held locks
  static thread_local ReadSet read_set;
  static thread_local WriteSet write_set;
};

}  // namespace leanstore::transaction::svcc
