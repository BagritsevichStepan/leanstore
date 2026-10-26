#pragma once

#include "common/typedefs.h"
#include "leanstore/kv_interface.h"
#include "transaction/svcc/lock_state.h"

#include "parallel_hashmap/phmap.h"

#include <span>
#include <string>
#include <string_view>

namespace leanstore::transaction::svcc {

/**
 * @brief Lock table of a single page.
 */
class PageLockTable {
 private:
  static constexpr size_t MAX_FREE_ENTRIES = 120;

  using Map = phmap::flat_hash_map<std::string, LockState>;

  static auto AsKey(std::span<const u8> key) -> std::string_view;

 public:
  /* Acquires the lock of key, created if absent. holds_shared: an upgrade of the caller's shared lock */
  auto TryAcquire(std::span<const u8> key, wid_t worker_id, bool exclusive, bool holds_shared = false) -> bool;
  /* Releases the lock of key, which must be held */
  void Release(std::span<const u8> key, bool exclusive);
  /* Releases the lock of key if it is in the table. false if key is absent */
  auto TryRelease(std::span<const u8> key, bool exclusive) -> bool;

  auto IsEmpty() const -> bool;

  /* Leaf split: hand the locks of every key > separator over to the new right sibling */
  void MoveUpperRange(std::span<const u8> separator, const ComparisonLambda &cmp, PageLockTable &right);
  /* Leaf merge of this (left) leaf into `right`: hand every lock over to `right` */
  void MergeInto(PageLockTable &right);

  /* Erases the free entries and shrinks the table, e.g. before it is moved to the store */
  void Compact();

 private:
  void ReleaseLock(Map::iterator lock, bool exclusive);
  void EraseFreeEntries();

  Map entries_;
  size_t free_entry_count_ = 0;  // entries whose lock is free
};

// Implementation
inline auto PageLockTable::AsKey(std::span<const u8> key) -> std::string_view {
  return {reinterpret_cast<const char *>(key.data()), key.size()};
}

inline auto PageLockTable::IsEmpty() const -> bool {
  return entries_.size() == free_entry_count_;
}

inline void PageLockTable::Compact() {
  EraseFreeEntries();
  entries_.rehash(0);
}

}  // namespace leanstore::transaction::svcc
