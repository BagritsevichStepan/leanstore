#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "leanstore/kv_interface.h"

#include <atomic>
#include <functional>
#include <span>
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
  std::atomic<u32> word_{0};
};

/**
 * @brief Lock table of a single page.
 *
 * Not thread-safe: the caller holds the latch of the page's PageLockState (see PageLockState::Guard).
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
  auto GetOrCreate(std::span<const u8> key) -> LockState *;

  /* Applies update_lock(LockState &) to the lock of `key`, which must be in the table; erases it once free */
  template <typename UpdateLockFn>
  void UpdateLockState(std::span<const u8> key, UpdateLockFn &&update_lock);

  auto Empty() const -> bool;

  /* Leaf split: hand the entries of every key > separator over to the new right sibling */
  void MoveUpperRange(std::span<const u8> separator, const ComparisonLambda &cmp, PageLockTable &right);
  /* Leaf merge of this (left) leaf into `right`: hand every entry over to `right` */
  void MergeInto(PageLockTable &right, std::span<const u8> left_upper_fence, const ComparisonLambda &cmp);

 private:
  static void Bury(Map::node_type &&node);

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
  auto prev_shared_count = word_.fetch_sub(1);
  Ensure(GetSharedCount(prev_shared_count) > 0);
}

inline void LockState::ReleaseExclusive() {
  auto prev_exclusive_owner = word_.fetch_and(SHARED_MASK);
  Ensure(GetOwnerId(prev_exclusive_owner) != NO_OWNER);
}

inline auto LockState::IsFree() const -> bool {
  return word_.load() == 0;
}

template <typename UpdateLockFn>
void PageLockTable::UpdateLockState(std::span<const u8> key, UpdateLockFn &&update_lock) {
  auto lock = entries_.find(std::string_view(reinterpret_cast<const char *>(key.data()), key.size()));
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
