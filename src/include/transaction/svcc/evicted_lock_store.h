#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "sync/hybrid_guard.h"
#include "sync/hybrid_latch.h"
#include "transaction/svcc/page_lock_table.h"

#include <span>
#include <unordered_map>

namespace leanstore::transaction::svcc {

/**
 * @brief Locks of evicted pages (SVCC in-page locking), one store for all trees of the database.
 *
 * A page's locks are keyed by (pid, key): the pid is unique in the whole buffer pool, no tree id is needed.
 *
 * The caller serializes all operations on the locks of one page: it holds the latch of that page's PageLockState.
 * The store only protects its own structure against operations on other pages.
 *
 * TODO(XXX): an in-memory map for now, a B-tree with its own small buffer pool later (same API)
 */
class EvictedLockStore {
 public:
  // Eviction of page
  void Spill(pageid_t pid, PageLockTable &table);
  // Restore of page
  void Restore(pageid_t pid, PageLockTable &table);

  /* Applies update_lock() to the lock of (pid, key) of the evicted page `pid`. The lock must be in the store */
  template <typename UpdateLockFn>
  void UpdateLockState(pageid_t pid, std::span<const u8> key, UpdateLockFn &&update_lock);

 private:
  sync::HybridLatch latch_;
  std::unordered_map<pageid_t, PageLockTable> pages_;  // only pages with locks
};

// Implementation
template <typename UpdateLockFn>
void EvictedLockStore::UpdateLockState(pageid_t pid, std::span<const u8> key, UpdateLockFn &&update_lock) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);

  auto page_iterator = pages_.find(pid);
  Ensure(page_iterator != pages_.end());

  auto& page_lock_table = page_iterator->second;
  page_lock_table.UpdateLockState(key, update_lock);
  if (page_lock_table.Empty()) {
    pages_.erase(page_iterator);
  }
}

}  // namespace leanstore::transaction::svcc
