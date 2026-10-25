#pragma once

#include "common/typedefs.h"
#include "sync/hybrid_latch.h"
#include "transaction/svcc/page_lock_table.h"

#include <string_view>
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

  /**
   * Releases the lock of (pid, key) of the evicted page pid, which must be in the store; erases it once free.
   * @return true if the store holds no more locks of the page
   */
  auto Release(pageid_t pid, std::string_view key, bool exclusive) -> bool;

 private:
  sync::HybridLatch latch_;
  std::unordered_map<pageid_t, PageLockTable> pages_;  // only pages with locks
};

}  // namespace leanstore::transaction::svcc
