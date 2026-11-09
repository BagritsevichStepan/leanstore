#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"

namespace leanstore::transaction::svcc {

/**
 * @brief 2PL lock state of a single key (No-Wait).
 */
struct LockState {
 private:
  static constexpr u32 SHARED_MASK = 0xFFFF;
  static constexpr u32 OWNER_SHIFT = 16;
  static constexpr u16 NO_OWNER = 0;

  auto GetSharedCount() const -> u32;
  auto GetOwnerId() const -> u16;

  auto TryAcquireShared(wid_t worker_id) -> bool;
  auto TryAcquireExclusive(wid_t worker_id, bool holds_shared) -> bool;

  void ReleaseShared();
  void ReleaseExclusive();

 public:  
  /* holds_shared: an upgrade of the caller's shared lock to an exclusive one */
  auto TryAcquire(wid_t worker_id, bool exclusive, bool holds_shared = false) -> bool;
  void Release(bool exclusive);

  /* Neither shared nor exclusive holders */
  auto IsFree() const -> bool;

  u32 word{0};
};

// Implementation
inline auto LockState::GetSharedCount() const -> u32 {
  return word & SHARED_MASK;
}

inline auto LockState::GetOwnerId() const -> u16 {
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
  Ensure(GetSharedCount() > 0);
  word--;
}

inline void LockState::ReleaseExclusive() {
  Ensure(GetOwnerId() != NO_OWNER);
  word &= SHARED_MASK;
}

inline auto LockState::IsFree() const -> bool {
  return word == 0;
}

}  // namespace leanstore::transaction::svcc
