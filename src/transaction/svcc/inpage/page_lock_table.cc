#include "transaction/svcc/inpage/page_lock_table.h"
#include "common/exceptions.h"

#include <algorithm>
#include <iterator>
#include <type_traits>

namespace leanstore::transaction::svcc::inpage {

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

auto PageLockTable::TryAcquire(std::span<const u8> key, LockParams params, bool holds_shared) -> LockResult {
  auto key_view = AsKey(key);

  return std::visit(
    [&](auto &entries) {
      using LockState = typename std::decay_t<decltype(entries)>::mapped_type;

      auto lock = entries.lazy_emplace(key_view, [&](const auto &construct) {
        construct(std::string(key_view), LockState());
      });

      return lock->second.TryAcquire(params, holds_shared);
    },
    entries_);
}

auto PageLockTable::TryRelease(std::span<const u8> key, timestamp_t txn_ts, bool exclusive)
  -> std::optional<std::vector<wid_t>> {
  auto key_view = AsKey(key);

  return std::visit(
    [&](auto &entries) -> std::optional<std::vector<wid_t>> {
      using LockState = typename std::decay_t<decltype(entries)>::mapped_type;

      auto lock = entries.find(key_view);
      if (lock == entries.end()) {
        return std::nullopt;
      }

      std::vector<wid_t> workers_to_wake;
      if constexpr (std::is_same_v<LockState, NoWaitLockState>) {
        lock->second.Release(exclusive);
      } else {
        workers_to_wake = lock->second.Release(txn_ts);
      }

      if (lock->second.IsFree()) {
        entries.erase(lock);
      }

      return workers_to_wake;
    },
    entries_);
}

void PageLockTable::MoveLocksAboveInto(std::span<const u8> separator, const ComparisonLambda &cmp, PageLockTable &right) {
  auto separator_view = AsKey(separator);

  std::visit(
    [&](auto &entries) {
      auto &right_entries = std::get<std::decay_t<decltype(entries)>>(right.entries_);

      for (auto it = entries.begin(); it != entries.end();) {
        auto next = std::next(it);

        if (CompareKeys(it->first, separator_view, cmp) > 0) {
          auto result = right_entries.insert(entries.extract(it));
          Ensure(result.inserted);
        }

        it = next;
      }
    },
    entries_);
}

void PageLockTable::MergeInto(PageLockTable &right) {
  std::visit(
    [&](auto &entries) {
      auto &right_entries = std::get<std::decay_t<decltype(entries)>>(right.entries_);

      while (!entries.empty()) {
        auto result = right_entries.insert(entries.extract(entries.begin()));
        Ensure(result.inserted);
      }
    },
    entries_);
}

void PageLockTable::AddLock(std::span<const u8> key, std::span<const u8> state) {
  std::visit(
    [&](auto &entries) {
      using LockState = typename std::decay_t<decltype(entries)>::mapped_type;

      LockState lock = LockState::FromRawBytes(state);
      Ensure(!lock.IsFree());

      auto result = entries.try_emplace(std::string(AsKey(key)), std::move(lock));
      Ensure(result.second);
    },
    entries_);
}

}  // namespace leanstore::transaction::svcc::inpage
