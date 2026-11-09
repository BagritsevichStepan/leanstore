#include "transaction/svcc/evicted_lock_store.h"
#include "common/exceptions.h"

#include <endian.h>
#include <algorithm>
#include <cstring>

namespace leanstore::transaction::svcc {

namespace {

constexpr size_t PID_BYTES = sizeof(pageid_t);

auto PayloadToLockState(std::span<const u8> payload) -> LockState {
  Ensure(payload.size() == sizeof(u32));

  u32 word;
  std::memcpy(&word, payload.data(), sizeof(u32));
  
  return LockState{word};
}

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

auto IsKeyOfPage(std::span<const u8> key, pageid_t pid) -> bool {
  auto encoded_pid = EncodePageId(pid);
  return std::memcmp(key.data(), &encoded_pid, PID_BYTES) == 0;
}

} // namespace

void EvictedLockStore::Spill(pageid_t pid, PageLockTable &table) {
  table.ForEachHeldLock([&](std::span<const u8> key, const LockState &lock) {
    auto store_key = StoreKey(pid, key);
    auto insertion_result = tree_.Insert(store_key, {reinterpret_cast<const u8 *>(&lock.word), sizeof(lock.word)});
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
      table.AddLock(stored_key.subspan(PID_BYTES), PayloadToLockState(payload));
      return true;
    });

  Ensure(extraction_result == OpResult::OK);
}

void EvictedLockStore::Release(pageid_t pid, std::span<const u8> key, bool exclusive) {
  auto stored_key = StoreKey(pid, key);

  bool lock_is_free = false;

  auto update_result = tree_.UpdateInPlace(
    stored_key,
    [&](std::span<u8> payload) {
      LockState lock = PayloadToLockState(payload);
      lock.Release(exclusive);
      std::memcpy(payload.data(), &lock.word, sizeof(lock.word));
      lock_is_free = lock.IsFree();
    },
    nullptr);

  Ensure(update_result == OpResult::OK);

  if (lock_is_free) {
    auto remove_result = tree_.Remove(stored_key);
    Ensure(remove_result == OpResult::OK);
  }
}

}  // namespace leanstore::transaction::svcc
