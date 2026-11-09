#include "transaction/svcc/page_lock_table.h"
#include "common/exceptions.h"

#include <algorithm>
#include <iterator>

namespace leanstore::transaction::svcc {

namespace {

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

auto PageLockTable::TryAcquire(std::span<const u8> key, wid_t worker_id, bool exclusive, bool holds_shared) -> bool {
  auto key_view = AsKey(key);

  auto lock = entries_.lazy_emplace(key_view, [&](const auto &construct) {
    construct(std::string(key_view), LockState());
  });

  return lock->second.TryAcquire(worker_id, exclusive, holds_shared);
}

void PageLockTable::Release(std::span<const u8> key, bool exclusive) {
  auto lock = entries_.find(AsKey(key));
  Ensure(lock != entries_.end());
  ReleaseLock(lock, exclusive);
}

auto PageLockTable::TryRelease(std::span<const u8> key, bool exclusive) -> bool {
  auto lock = entries_.find(AsKey(key));
  if (lock == entries_.end()) {
    return false;
  }

  ReleaseLock(lock, exclusive);
  return true;
}

void PageLockTable::ReleaseLock(Map::iterator lock, bool exclusive) {
  lock->second.Release(exclusive);
  if (lock->second.IsFree()) {
    entries_.erase(lock);
  }
}

void PageLockTable::MoveUpperRange(std::span<const u8> separator, const ComparisonLambda &cmp,
                                   PageLockTable &right) {
  auto separator_view = AsKey(separator);

  for (auto it = entries_.begin(); it != entries_.end();) {
    auto next = std::next(it);

    if (CompareKeys(it->first, separator_view, cmp) > 0) {
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
