#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "leanstore/kv_interface.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace leanstore::transaction::svcc {

/**
 * @brief 2PL lock state of a single key (No-Wait).
 */
class LockState {
 private:
  static constexpr u32 SHARED_MASK = 0xFFFF;
  static constexpr u32 OWNER_SHIFT = 16;
  static constexpr u16 NO_OWNER = 0;

  static auto GetSharedCount(u32 word) -> u32;
  static auto GetOwnerId(u32 word) -> u16;

 public:
  auto TryAcquireShared(wid_t worker_id) -> bool;
  auto TryAcquireExclusive(wid_t worker_id, bool holds_shared) -> bool;

  void ReleaseShared();
  void ReleaseExclusive();

  /* Neither shared nor exclusive holders */
  auto IsFree() const -> bool;

 private:
  u32 word_{0};
};

/**
 * @brief Lock table of a single page.
 */
class PageLockTable {
 private:
  struct KeyHash {
    using is_transparent = void;

    auto operator()(std::string_view key) const -> size_t {
      return std::hash<std::string_view>{}(key);
    }
  };

  using Map = std::unordered_map<std::string, LockState, KeyHash, std::equal_to<>>;

 public:
  /* Acquires the lock of key, created if absent. holds_shared: an upgrade of the caller's shared lock */
  auto TryAcquire(std::string_view key, wid_t worker_id, bool exclusive, bool holds_shared = false) -> bool;
  /* Releases the lock of key, which must be in the table; erases it once free */
  void Release(std::string_view key, bool exclusive);
  /* Releases the lock of key if it is in the table; erases it once free. false if key is absent */
  auto TryRelease(std::string_view key, bool exclusive) -> bool;

  /* Applies update_lock() to the lock of key, which must be in the table; erases it once free */
  template <typename UpdateLockFn>
  void UpdateLockState(std::string_view key, UpdateLockFn &&update_lock);

  auto Empty() const -> bool;

  /* Leaf split: hand the entries of every key > separator over to the new right sibling */
  void MoveUpperRange(std::string_view separator, const ComparisonLambda &cmp, PageLockTable &right);
  /* Leaf merge of this (left) leaf into `right`: hand every entry over to `right` */
  void MergeInto(PageLockTable &right);

 private:
  Map entries_;
};

// Implementation
inline auto LockState::GetSharedCount(u32 word) -> u32 {
  return word & SHARED_MASK;
}

inline auto LockState::GetOwnerId(u32 word) -> u16 {
  return static_cast<u16>(word >> OWNER_SHIFT);
}

inline void LockState::ReleaseShared() {
  Ensure(GetSharedCount(word_) > 0);
  word_--;
}

inline void LockState::ReleaseExclusive() {
  Ensure(GetOwnerId(word_) != NO_OWNER);
  word_ &= SHARED_MASK;
}

inline auto LockState::IsFree() const -> bool {
  return word_ == 0;
}

template <typename UpdateLockFn>
void PageLockTable::UpdateLockState(std::string_view key, UpdateLockFn &&update_lock) {
  auto lock = entries_.find(key);
  Ensure(lock != entries_.end());

  update_lock(lock->second);
  
  if (lock->second.IsFree()) {
    entries_.erase(lock);
  }
}

inline auto PageLockTable::Empty() const -> bool {
  return entries_.empty();
}

}  // namespace leanstore::transaction::svcc
