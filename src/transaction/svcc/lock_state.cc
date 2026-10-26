#include "transaction/svcc/lock_state.h"

namespace leanstore::transaction::svcc {

namespace {

auto GetOwnerIdFromWorkerId(wid_t worker_id) -> u16 {
  return static_cast<u16>(worker_id + 1);
}

} // namespace

auto LockState::TryAcquireShared(wid_t worker_id) -> bool {
  const auto owner_id = GetOwnerIdFromWorkerId(worker_id);

  const auto exclusive_owner = GetOwnerId(word_);
  if (exclusive_owner != NO_OWNER && exclusive_owner != owner_id) {
    return false;
  }

  Ensure(GetSharedCount(word_) < SHARED_MASK);
  word_++;
  return true;
}

auto LockState::TryAcquireExclusive(wid_t worker_id, bool holds_shared) -> bool {
  const auto owner_id = GetOwnerIdFromWorkerId(worker_id);

  const auto exclusive_owner = GetOwnerId(word_);
  if (exclusive_owner == owner_id) {
    return true;
  }
  if (exclusive_owner != NO_OWNER) {
    return false;
  }

  const u32 expected_shared_count = holds_shared ? 1 : 0;
  if (GetSharedCount(word_) > expected_shared_count) {
    return false;
  }

  Ensure(GetSharedCount(word_) == expected_shared_count);

  // The shared lock of an upgrade is consumed by the exclusive one
  word_ = static_cast<u32>(owner_id) << OWNER_SHIFT;
  return true;
}

}  // namespace leanstore::transaction::svcc
