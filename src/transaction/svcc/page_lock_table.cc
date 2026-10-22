#include "transaction/svcc/page_lock_table.h"
#include "common/exceptions.h"

#include <algorithm>

namespace leanstore::transaction::svcc {

namespace {

auto GetOwnerIdFromWorkerId(wid_t worker_id) -> u16 {
  return static_cast<u16>(worker_id + 1);
}

auto CompareKeys(std::string_view lhs, std::string_view rhs, const ComparisonLambda &cmp) -> int {
  auto ret = cmp.func(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
  if (ret != 0) {
    return ret;
  }

  if (lhs.size() == rhs.size()) {
    return 0;
  }

  return (lhs.size() < rhs.size()) ? -1 : 1;
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

auto PageLockTable::TryAcquire(std::string_view key, wid_t worker_id, bool exclusive, bool holds_shared) -> bool {
  auto lock = entries_.find(key);
  if (lock == entries_.end()) {
    lock = entries_.try_emplace(std::string(key)).first;
  }

  if (exclusive) {
    return lock->second.TryAcquireExclusive(worker_id, holds_shared);
  }
  
  return lock->second.TryAcquireShared(worker_id);
}

void PageLockTable::Release(std::string_view key, bool exclusive) {
  UpdateLockState(key, [&](LockState &lock) {
    if (exclusive) {
      lock.ReleaseExclusive();
    } else {
      lock.ReleaseShared();
    }
  });
}

auto PageLockTable::TryRelease(std::string_view key, bool exclusive) -> bool {
  auto lock = entries_.find(key);
  if (lock == entries_.end()) {
    return false;
  }

  if (exclusive) {
    lock->second.ReleaseExclusive();
  } else {
    lock->second.ReleaseShared();
  }

  if (lock->second.IsFree()) {
    entries_.erase(lock);
  }
  
  return true;
}

void PageLockTable::MoveUpperRange(std::string_view separator, const ComparisonLambda &cmp,
                                   PageLockTable &right) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto next = std::next(it);
    if (CompareKeys(it->first, separator, cmp) > 0) {
      auto result = right.entries_.insert(entries_.extract(it));
      Ensure(result.inserted);
    }
    it = next;
  }
}

void PageLockTable::MergeInto(PageLockTable &right) {
  while (!entries_.empty()) {
    auto result = right.entries_.insert(entries_.extract(entries_.begin()));
    Ensure(result.inserted);
  }
}

}  // namespace leanstore::transaction::svcc
