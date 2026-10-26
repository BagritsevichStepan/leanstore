#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"

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
  /* holds_shared: an upgrade of the caller's shared lock to an exclusive one */
  auto TryAcquire(wid_t worker_id, bool exclusive, bool holds_shared = false) -> bool;
  void Release(bool exclusive);

  /* Neither shared nor exclusive holders */
  auto IsFree() const -> bool;

 private:
  auto TryAcquireShared(wid_t worker_id) -> bool;
  auto TryAcquireExclusive(wid_t worker_id, bool holds_shared) -> bool;

  void ReleaseShared();
  void ReleaseExclusive();

  u32 word_{0};
};

// Implementation
inline auto LockState::GetSharedCount(u32 word) -> u32 {
  return word & SHARED_MASK;
}

inline auto LockState::GetOwnerId(u32 word) -> u16 {
  return static_cast<u16>(word >> OWNER_SHIFT);
}

inline auto LockState::TryAcquire(wid_t worker_id, bool exclusive, bool holds_shared) -> bool {
  return exclusive ? TryAcquireExclusive(worker_id, holds_shared) : TryAcquireShared(worker_id);
}

inline void LockState::Release(bool exclusive) {
  if (exclusive) {
    ReleaseExclusive();
  } else {
    ReleaseShared();
  }
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

}  // namespace leanstore::transaction::svcc
