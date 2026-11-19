#pragma once

#include "transaction/svcc/inpage/detail/timestamp_sorted_list.h"
#include "transaction/svcc/inpage/lock_policy.h"

#include <span>
#include <vector>

namespace leanstore::transaction::svcc::inpage {

/**
 * @brief The mode a key is held in, or asked for.
 */
enum class LockMode : u8 {
  NONE,
  SHARED,
  EXCLUSIVE,
  UPGRADE,
};

struct OwnerNode {
  timestamp_t ts{0};
};

struct WaiterNode {
  timestamp_t ts{0};
  wid_t worker_id{0};
  LockMode mode{LockMode::NONE};
};

/**
 * @brief Wait-Die: a worker older than the holders waits for them, a younger one gives up.
 */
class WaitDieLockState {
 public:
  /* holds_shared: an upgrade of the caller's own shared lock to an exclusive one */
  auto TryAcquire(LockParams params, bool holds_shared) -> LockResult;
  auto Release(timestamp_t txn_ts) -> std::vector<wid_t>;
  auto IsFree() const -> bool;

  auto ToRawBytes() const -> std::vector<u8>;
  static auto FromRawBytes(std::span<const u8> payload) -> WaitDieLockState;

 private:
  auto TakeOwnership(timestamp_t txn_ts, LockMode mode) -> void;
  auto GetOldestOwnerTimestamp() const -> timestamp_t;
  auto UpgradeToExclusive(LockParams params) -> LockResult;
  auto PromoteWaiters() -> std::vector<wid_t>;

  LockMode owners_mode_{LockMode::NONE};
  detail::TimestampSortedList<OwnerNode> owners_;
  detail::TimestampSortedList<WaiterNode> waiters_;
};

// Implementation
inline auto WaitDieLockState::Release(timestamp_t txn_ts) -> std::vector<wid_t> {
  owners_.Remove(txn_ts);

  if (owners_.IsEmpty()) {
    owners_mode_ = LockMode::NONE;
  }

  return PromoteWaiters();
}

inline auto WaitDieLockState::IsFree() const -> bool {
  Ensure(owners_mode_ != LockMode::NONE || (owners_.IsEmpty() && waiters_.IsEmpty()));
  return owners_mode_ == LockMode::NONE;
}

}  // namespace leanstore::transaction::svcc::inpage
