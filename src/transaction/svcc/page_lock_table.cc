#include "transaction/svcc/page_lock_table.h"
#include "common/exceptions.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace leanstore::transaction::svcc {

namespace {

auto GetOwnerIdFromWorkerId(wid_t worker_id) -> u16 {
  return static_cast<u16>(worker_id + 1);
}

auto ToView(std::span<const u8> key) -> std::string_view {
  return {reinterpret_cast<const char *>(key.data()), key.size()};
}

auto CompareKeys(std::string_view lhs, std::span<const u8> rhs, const ComparisonLambda &cmp) -> int {
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

  auto word = word_.load();
  while (true) {
    const auto exclusive_owner = GetOwnerId(word);
    if (exclusive_owner != NO_OWNER && exclusive_owner != owner_id) {
      return false;
    }

    Ensure(GetSharedCount(word) < SHARED_MASK);

    if (word_.compare_exchange_weak(word, word + 1)) {
      return true;
    }
  }
}

auto LockState::TryAcquireExclusive(wid_t worker_id, bool holds_shared) -> bool {
  const auto owner_id = GetOwnerIdFromWorkerId(worker_id);

  auto word = word_.load();
  while (true) {
    const auto exclusive_owner = GetOwnerId(word);
    if (exclusive_owner == owner_id) {
      return true;
    }
    if (exclusive_owner != NO_OWNER) {
      return false;
    }

    const u32 expected_shared_count = holds_shared ? 1 : 0;
    if (GetSharedCount(word) > expected_shared_count) {
      return false;
    }

    Ensure(GetSharedCount(word) == expected_shared_count);

    const u32 new_exclusive_owner = static_cast<u32>(owner_id) << OWNER_SHIFT;
    if (word_.compare_exchange_weak(word, new_exclusive_owner)) {
      return true;
    }
  }
}


auto PageLockTable::GetOrCreate(std::span<const u8> key) -> LockState * {
  const auto as_view = ToView(key);

  auto it = entries_.find(as_view);
  if (it == entries_.end()) {
    it = entries_.try_emplace(std::string(as_view)).first;
  }

  return &it->second;
}

void PageLockTable::MoveUpperRange(std::span<const u8> separator, const ComparisonLambda &cmp,
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

void PageLockTable::MergeInto(PageLockTable &right, std::span<const u8> left_upper_fence,
                              const ComparisonLambda &cmp) {
  while (!entries_.empty()) {
    auto node = entries_.extract(entries_.begin());

    const bool own_key = CompareKeys(node.key(), left_upper_fence, cmp) <= 0;
    auto existing = right.entries_.find(std::string_view(node.key()));

    if (existing == right.entries_.end()) {
      right.entries_.insert(std::move(node));
    } else if (own_key) {
      Bury(right.entries_.extract(existing));
      right.entries_.insert(std::move(node));
    } else {
      Bury(std::move(node));
    }
  }
}

/**
 * Tmp method to avoid memory leaks: the node is removed from the map, but its LockState object is not
 */
void PageLockTable::Bury(Map::node_type &&node) {
  static std::mutex graveyard_latch;
  static std::vector<Map::node_type> graveyard;
  std::lock_guard guard(graveyard_latch);
  graveyard.push_back(std::move(node));
}

}  // namespace leanstore::transaction::svcc
