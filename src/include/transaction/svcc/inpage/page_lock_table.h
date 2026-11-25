#pragma once

#include "common/typedefs.h"
#include "leanstore/kv_interface.h"
#include "transaction/svcc/inpage/lock_policy.h"
#include "transaction/svcc/inpage/no_wait_lock_state.h"
#include "transaction/svcc/inpage/wait_die_lock_state.h"

#include "parallel_hashmap/phmap.h"

#include <cstring>
#include <span>
#include <string>
#include <optional>
#include <string_view>
#include <variant>

namespace leanstore::transaction::svcc::inpage {

/**
 * @brief Lock table of a single page. It holds only the held locks: a lock leaves it once it is free.
 */
class PageLockTable {
 private:
  template <class LockState>
  using Entries = phmap::flat_hash_map<std::string, LockState>;

  using NoWaitEntries = Entries<NoWaitLockState>;
  using WaitDieEntries = Entries<WaitDieLockState>;

 public:
  PageLockTable();

  /* Acquires the lock of key, created if absent */
  auto TryAcquire(std::span<const u8> key, LockParams params, bool holds_shared = false) -> LockResult;

  /* Releases the lock of key, which must be held. Returns the waiters to wake up. */
  auto Release(std::span<const u8> key, timestamp_t txn_ts, bool exclusive) -> std::vector<wid_t>;
  /* Releases the lock of key if it is in the table. Nullopt if key is absent */
  auto TryRelease(std::span<const u8> key, timestamp_t txn_ts, bool exclusive) -> std::optional<std::vector<wid_t>>;

  /* Leaf split: hand the locks of every key > separator over to the new right sibling */
  void MoveLocksAboveInto(std::span<const u8> separator, const ComparisonLambda &cmp, PageLockTable &right);
  /* Leaf merge of this (left) leaf into right: move every held lock to right lock table */
  void MergeInto(PageLockTable &right);

  /* Methods for the EvictedLockStore */
  /* Used when spilling locks into the EvictedLockStore */
  template <typename Fn>
  void ForEachHeldLock(Fn &&fn) const;
  /* Used when restoring locks from the EvictedLockStore */
  void AddLock(std::span<const u8> key, std::span<const u8> state);

  auto IsEmpty() const -> bool;

 private:
  static auto AsKey(std::span<const u8> key) -> std::string_view;

  std::variant<NoWaitEntries, WaitDieEntries> entries_;
};

// Implementation
inline PageLockTable::PageLockTable() {
  if (IsNoWait()) {
    entries_.emplace<NoWaitEntries>();
  } else {
    entries_.emplace<WaitDieEntries>();
  }
}

inline auto PageLockTable::Release(std::span<const u8> key, timestamp_t txn_ts, bool exclusive) -> std::vector<wid_t> {
  auto workers_to_wake = TryRelease(key, txn_ts, exclusive);
  Ensure(workers_to_wake.has_value());
  return std::move(workers_to_wake).value();
}

inline auto PageLockTable::AsKey(std::span<const u8> key) -> std::string_view {
  return {reinterpret_cast<const char *>(key.data()), key.size()};
}

inline auto PageLockTable::IsEmpty() const -> bool {
  return std::visit([](const auto &entries) { return entries.empty(); }, entries_);
}

template <typename Fn>
void PageLockTable::ForEachHeldLock(Fn &&fn) const {
  std::visit(
    [&](const auto &entries) {
      for (const auto &[key, lock] : entries) {
        std::vector<u8> lock_raw_bytes = lock.ToRawBytes();
        fn(std::span<const u8>(reinterpret_cast<const u8 *>(key.data()), key.size()),
           std::span<const u8>(lock_raw_bytes));
      }
    },
    entries_);
}

}  // namespace leanstore::transaction::svcc::inpage
