#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "leanstore/kv_interface.h"
#include "transaction/svcc/lock_state.h"

#include "parallel_hashmap/phmap.h"

#include <span>
#include <string>
#include <string_view>

namespace leanstore::transaction::svcc {

/**
 * @brief Lock table of a single page. It holds only the held locks: a lock leaves it once it is free.
 */
class PageLockTable {
 private:
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

  template <typename Fn>
  void ForEachHeldLock(Fn &&fn) const;
  void AddLock(std::span<const u8> key, const LockState &lock);

 private:
  void ReleaseLock(Map::iterator lock, bool exclusive);

  Map entries_;
};

// Implementation
inline auto PageLockTable::AsKey(std::span<const u8> key) -> std::string_view {
  return {reinterpret_cast<const char *>(key.data()), key.size()};
}

inline auto PageLockTable::IsEmpty() const -> bool {
  return entries_.empty();
}

template <typename Fn>
void PageLockTable::ForEachHeldLock(Fn &&fn) const {
  for (const auto &[key, lock] : entries_) {
    fn(std::span<const u8>(reinterpret_cast<const u8 *>(key.data()), key.size()), lock);
  }
}

inline void PageLockTable::AddLock(std::span<const u8> key, const LockState &lock) {
  Ensure(!lock.IsFree());
  auto result = entries_.try_emplace(std::string(AsKey(key)), lock);
  Ensure(result.second);
}

}  // namespace leanstore::transaction::svcc
