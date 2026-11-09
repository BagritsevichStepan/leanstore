#pragma once

#include "common/typedefs.h"
#include "storage/btree/tree.h"
#include "transaction/svcc/page_lock_table.h"

#include <span>

namespace leanstore::transaction::svcc {

/**
 * @brief Locks of evicted pages, one store for all trees of the database.
 */
class EvictedLockStore {
 public:
  explicit EvictedLockStore(buffer::BufferManager *buffer);

  // Eviction of page
  void Spill(pageid_t pid, PageLockTable &table);
  // Restore of page
  void Restore(pageid_t pid, PageLockTable &table);

  /* Releases the lock of (pid, key) of the evicted page pid, which must be in the store; erases it once free */
  void Release(pageid_t pid, std::span<const u8> key, bool exclusive);

 private:
  storage::BTree tree_;
};

inline EvictedLockStore::EvictedLockStore(buffer::BufferManager *buffer) : tree_(buffer, 0, true) {}

}  // namespace leanstore::transaction::svcc
