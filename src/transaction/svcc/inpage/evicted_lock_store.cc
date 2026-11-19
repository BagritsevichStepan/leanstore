#include "transaction/svcc/inpage/evicted_lock_store.h"
#include "common/exceptions.h"
#include "transaction/svcc/inpage/no_wait_lock_state.h"
#include "transaction/svcc/inpage/wait_die_lock_state.h"

#include <endian.h>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace leanstore::transaction::svcc::inpage {

namespace {

constexpr size_t PID_BYTES = sizeof(pageid_t);

/* The pid in big-endian, the most significant byte first: memcmp orders the encoded pids like the pids */
auto EncodePageId(pageid_t pid) -> pageid_t {
  return htobe64(pid);
}

auto StoreKey(pageid_t pid, std::span<const u8> key) -> std::vector<u8> {
  auto encoded_pid = EncodePageId(pid);
  std::vector<u8> stored_key(PID_BYTES + key.size());

  std::memcpy(stored_key.data(), &encoded_pid, PID_BYTES);
  std::copy(key.begin(), key.end(), stored_key.begin() + PID_BYTES);

  return stored_key;
}

template <class LockState>
auto ReleaseSpilledLock(std::span<u8> lock_payload, timestamp_t txn_ts, bool exclusive)
  -> std::pair<std::vector<wid_t>, bool> {
  LockState lock = LockState::FromRawBytes(lock_payload);

  std::vector<wid_t> workers_to_wake;
  if constexpr (std::is_same_v<LockState, NoWaitLockState>) {
    lock.Release(exclusive);
  } else {
    workers_to_wake = lock.Release(txn_ts);
  }

  const bool lock_is_free = lock.IsFree();

  auto lock_raw_bytes = lock.ToRawBytes();
  Ensure(lock_raw_bytes.size() <= lock_payload.size());
  std::memcpy(lock_payload.data(), lock_raw_bytes.data(), lock_raw_bytes.size());

  return {std::move(workers_to_wake), lock_is_free};
}

auto IsKeyOfPage(std::span<const u8> key, pageid_t pid) -> bool {
  auto encoded_pid = EncodePageId(pid);
  return std::memcmp(key.data(), &encoded_pid, PID_BYTES) == 0;
}

} // namespace

void EvictedLockStore::Spill(pageid_t pid, PageLockTable &table) {
  table.ForEachHeldLock([&](std::span<const u8> key, std::span<const u8> state) {
    auto store_key = StoreKey(pid, key);
    auto insertion_result = tree_.Insert(store_key, state);
    Ensure(insertion_result == OpResult::OK);
  });
}

void EvictedLockStore::Restore(pageid_t pid, PageLockTable &table) {
  Ensure(table.IsEmpty());

  auto prefix = StoreKey(pid, {});

  auto extraction_result = tree_.ExtractAscending(
    prefix,
    [&](std::span<const u8> stored_key) {
      return IsKeyOfPage(stored_key, pid);
    },
    [&](std::span<u8> stored_key, std::span<u8> payload) {
      table.AddLock(stored_key.subspan(PID_BYTES), payload);
      return true;
    });

  Ensure(extraction_result == OpResult::OK);
}

auto EvictedLockStore::Release(pageid_t pid, std::span<const u8> key, timestamp_t txn_ts, bool exclusive)
  -> std::vector<wid_t> {
  auto stored_key = StoreKey(pid, key);

  std::vector<wid_t> workers_to_wake;
  bool lock_is_free = false;

  auto update_result = tree_.UpdateInPlace(
    stored_key,
    [&](std::span<u8> payload) {
      auto released = IsNoWait() ? ReleaseSpilledLock<NoWaitLockState>(payload, txn_ts, exclusive)
                                 : ReleaseSpilledLock<WaitDieLockState>(payload, txn_ts, exclusive);
      
      workers_to_wake = std::move(released.first);
      lock_is_free = released.second;
    },
    nullptr);

  Ensure(update_result == OpResult::OK);

  if (lock_is_free) {
    auto remove_result = tree_.Remove(stored_key);
    Ensure(remove_result == OpResult::OK);
  }

  return workers_to_wake;
}

}  // namespace leanstore::transaction::svcc::inpage
