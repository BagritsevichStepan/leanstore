#include "transaction/svcc/inpage_lock_manager.h"
#include "common/exceptions.h"
#include "leanstore/leanstore.h"

namespace leanstore::transaction::svcc {

namespace {

auto GetKeySpan(const LockableTuple *key) -> std::span<const u8> {
  return {key->key, key->key_len};
}

} // namespace

thread_local std::unordered_set<LockState *> InPageLockManager::read_set;
thread_local std::unordered_map<LockState *, InPageLockManager::WriteEntry> InPageLockManager::write_set;

bool InPageLockManager::EmptyLocalSet() {
  return read_set.empty() && write_set.empty();
}

void InPageLockManager::ReleaseAllLocks([[maybe_unused]] timestamp_t txn_ts, const WriteSetCallback &write_set_cb) {
  for (auto state : read_set) {
    state->ReleaseShared();
  }
  read_set.clear();

  std::erase_if(write_set, [&](auto &write_set_entry) {
    auto &[lock_state, entry] = write_set_entry;
    write_set_cb(entry.tuple, INVALID_TS, std::span<const u8>(entry.undo_payload));
    lock_state->ReleaseExclusive();
    LockableTuple::Release(entry.tuple);
    return true;
  });
}

bool InPageLockManager::TryLockShared([[maybe_unused]] timestamp_t txn_ts, const LockableTuple *key,
                                      PageLockTable *page_locks) {
  Ensure(page_locks != nullptr);

  auto lock_state = page_locks->GetOrCreate(GetKeySpan(key));
  if (read_set.contains(lock_state) || write_set.contains(lock_state)) {
    return true;
  }

  if (!lock_state->TryAcquireShared(LeanStore::worker_thread_id)) {
    return false;
  }

  read_set.insert(lock_state);
  return true;
}

bool InPageLockManager::TryLock([[maybe_unused]] timestamp_t txn_ts, [[maybe_unused]] timestamp_t undo_ts,
                                std::span<u8> undo_payload, const LockableTuple *key, PageLockTable *page_locks) {
  Ensure(page_locks != nullptr);

  auto lock_state = page_locks->GetOrCreate(GetKeySpan(key));
  if (write_set.contains(lock_state)) {
    return true;
  }

  // Upgrade if we already hold a shared lock on this key
  auto it = read_set.find(lock_state);
  const bool holds_shared = (it != read_set.end());
  if (!lock_state->TryAcquireExclusive(LeanStore::worker_thread_id, holds_shared)) {
    return false;
  }

  if (holds_shared) {
    read_set.erase(it);
  }

  write_set.emplace(lock_state, WriteEntry{LockableTuple::Constructor(*key),
                                           std::vector<u8>(undo_payload.begin(), undo_payload.end())});
  
  return true;
}

void InPageLockManager::Unlock([[maybe_unused]] timestamp_t txn_ts, [[maybe_unused]] const LockableTuple *key) {
  UnreachableCode();
}

void InPageLockManager::UnlockShared([[maybe_unused]] timestamp_t txn_ts, [[maybe_unused]] const LockableTuple *key) {
  UnreachableCode();
}

}  // namespace leanstore::transaction::svcc
