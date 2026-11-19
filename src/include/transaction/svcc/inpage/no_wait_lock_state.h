#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "transaction/svcc/inpage/lock_policy.h"

#include <cstring>
#include <span>
#include <vector>

namespace leanstore::transaction::svcc::inpage {

/**
 * @brief Lock policy: No-Wait. A worker that cannot acquire a lock immediately gives up and aborts.
 */
class NoWaitLockState {
 public:
  /* holds_shared: an upgrade of the caller's own shared lock to an exclusive one */
  auto TryAcquire(LockParams params, bool exclusive, bool holds_shared) -> LockResult;
  void Release(bool exclusive);
  auto IsFree() const -> bool;

  auto ToRawBytes() const -> std::vector<u8>;
  static auto FromRawBytes(std::span<const u8> payload) -> NoWaitLockState;

 private:
  static constexpr u32 EXCLUSIVE_OWNER_SHIFT = 16;
  static constexpr u16 NO_EXCLUSIVE_OWNER = 0;
  static constexpr u32 SHARED_COUNT_MASK = 0xFFFF;

  auto TryAcquireShared(u16 owner_id) -> bool;
  auto TryAcquireExclusive(u16 owner_id, bool holds_shared) -> bool;

  auto IsOwnedExclusively() const -> bool;
  auto IsOwnedExclusivelyBy(u16 owner_id) const -> bool;

  auto GetSharedCount() const -> u32;
  auto GetExclusiveOwnerId() const -> u16;

  static auto GetOwnerId(wid_t worker_id) -> u16;

  u32 word_{0};
};

// Implementation
inline auto NoWaitLockState::TryAcquire(LockParams params, bool exclusive, bool holds_shared) -> LockResult {
  const auto owner_id = GetOwnerId(params.worker_id);
  const bool acquired = exclusive ? TryAcquireExclusive(owner_id, holds_shared) : TryAcquireShared(owner_id);
  return acquired ? LockResult::ACQUIRED : LockResult::ABORT;
}

inline void NoWaitLockState::Release(bool exclusive) {
  if (exclusive) {
    Ensure(IsOwnedExclusively());
    word_ &= SHARED_COUNT_MASK;
    return;
  }

  Ensure(GetSharedCount() > 0);
  word_--;
}

inline auto NoWaitLockState::IsFree() const -> bool {
  return word_ == 0;
}

inline auto NoWaitLockState::TryAcquireShared(u16 owner_id) -> bool {
  if (IsOwnedExclusively() && !IsOwnedExclusivelyBy(owner_id)) {
    return false;
  }

  Ensure(GetSharedCount() < SHARED_COUNT_MASK);
  word_++;
  return true;
}

inline auto NoWaitLockState::TryAcquireExclusive(u16 owner_id, bool holds_shared) -> bool {
  if (IsOwnedExclusivelyBy(owner_id)) {
    return true;
  }

  if (IsOwnedExclusively()) {
    return false;
  }

  const u32 expected_shared_count = holds_shared ? 1 : 0;
  if (GetSharedCount() > expected_shared_count) {
    return false;
  }

  Ensure(GetSharedCount() == expected_shared_count);

  // The shared lock of an upgrade is consumed by the exclusive one
  word_ = static_cast<u32>(owner_id) << EXCLUSIVE_OWNER_SHIFT;
  return true;
}

inline auto NoWaitLockState::IsOwnedExclusively() const -> bool {
  return GetExclusiveOwnerId() != NO_EXCLUSIVE_OWNER;
}

inline auto NoWaitLockState::IsOwnedExclusivelyBy(u16 owner_id) const -> bool {
  return GetExclusiveOwnerId() == owner_id;
}

inline auto NoWaitLockState::GetSharedCount() const -> u32 {
  return word_ & SHARED_COUNT_MASK;
}

inline auto NoWaitLockState::GetExclusiveOwnerId() const -> u16 {
  return static_cast<u16>(word_ >> EXCLUSIVE_OWNER_SHIFT);
}

inline auto NoWaitLockState::GetOwnerId(wid_t worker_id) -> u16 {
  return static_cast<u16>(worker_id + 1);
}

inline auto NoWaitLockState::ToRawBytes() const -> std::vector<u8> {
  std::vector<u8> payload(sizeof(word_));
  std::memcpy(payload.data(), &word_, sizeof(word_));
  return payload;
}

inline auto NoWaitLockState::FromRawBytes(std::span<const u8> payload) -> NoWaitLockState {
  Ensure(payload.size() == sizeof(word_));

  NoWaitLockState state;
  std::memcpy(&state.word_, payload.data(), sizeof(state.word_));
  return state;
}

}  // namespace leanstore::transaction::svcc::inpage
