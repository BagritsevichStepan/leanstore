#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "leanstore/kv_interface.h"
#include "sync/hybrid_latch.h"

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

 private:
  std::atomic<u32> word_{0};
};

/**
 * @brief Lock table of a single B-tree leaf, hung off the buffer frame of that leaf
 *        (see BufferFrame::GetOrCreateLockTable()).
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

  /* Leaf split: hand the entries of every key > separator over to the new right sibling */
  void MoveUpperRange(std::span<const u8> separator, const ComparisonLambda &cmp, PageLockTable &right);
  /* Leaf merge of this (left) leaf into `right`: hand every entry over to `right` */
  void MergeInto(PageLockTable &right, std::span<const u8> left_upper_fence, const ComparisonLambda &cmp);

 private:
  static void Bury(Map::node_type &&node);

  sync::HybridLatch latch_;
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

}  // namespace leanstore::transaction::svcc
