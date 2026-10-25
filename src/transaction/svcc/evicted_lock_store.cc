#include "transaction/svcc/evicted_lock_store.h"
#include "common/exceptions.h"
#include "sync/hybrid_guard.h"

#include <utility>

namespace leanstore::transaction::svcc {

void EvictedLockStore::Spill(pageid_t pid, PageLockTable &table) {
  if (table.Empty()) {
    return;
  }

  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  // The table is moved as a whole: its locks are not copied one by one, the table is empty afterwards
  auto result = pages_.try_emplace(pid, std::exchange(table, PageLockTable()));
  Ensure(result.second);
}

void EvictedLockStore::Restore(pageid_t pid, PageLockTable &table) {
  Ensure(table.Empty());

  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  auto page = pages_.find(pid);
  if (page == pages_.end()) {
    return;
  }

  table = std::move(page->second);
  pages_.erase(page);
}

auto EvictedLockStore::Release(pageid_t pid, std::string_view key, bool exclusive) -> bool {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);

  auto page = pages_.find(pid);
  Ensure(page != pages_.end());

  auto &page_lock_table = page->second;
  page_lock_table.Release(key, exclusive);

  if (page_lock_table.Empty()) {
    pages_.erase(page);
    return true;
  }
  
  return false;
}

}  // namespace leanstore::transaction::svcc
