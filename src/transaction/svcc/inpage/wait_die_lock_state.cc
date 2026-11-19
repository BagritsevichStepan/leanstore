#include "transaction/svcc/inpage/wait_die_lock_state.h"
#include "common/exceptions.h"

#include <cstring>
#include <type_traits>
#include <utility>

namespace leanstore::transaction::svcc::inpage {

namespace {

constexpr size_t COUNT_BYTES = sizeof(u16);
constexpr size_t OWNER_BYTES = sizeof(timestamp_t);
constexpr size_t WAITER_BYTES = sizeof(timestamp_t) + sizeof(wid_t) + sizeof(LockMode);

/* Appends the fields one after the other, without the padding of their struct */
struct ByteWriter {
 public:
  explicit ByteWriter(size_t reservation_size) {
    payload.reserve(reservation_size);
  }

  template <class T>
    requires std::is_trivially_copyable_v<T>
  void Write(T value) {
    const auto offset = payload.size();
    payload.resize(offset + sizeof(T));
    std::memcpy(payload.data() + offset, &value, sizeof(T));
  }

  void WriteNode(const OwnerNode &node) {
    Write(node.ts);
  }

  void WriteNode(const WaiterNode &node) {
    Write(node.ts);
    Write(node.worker_id);
    Write(node.mode);
  }

  template <typename Node>
  void WriteList(const detail::TimestampSortedList<Node> &list) {
    Write(static_cast<u16>(list.Size()));
    list.ForEach([&](const Node &node) {
      WriteNode(node);
    });
  }

  std::vector<u8> payload;
};

/* Reads back what a ByteWriter wrote, consuming the bytes of every field it returns */
class ByteReader {
 public:
  explicit ByteReader(std::span<const u8> payload) : payload_(payload) {}

  template <class T>
    requires std::is_trivially_copyable_v<T>
  auto Read() -> T {
    Ensure(payload_.size() >= sizeof(T));

    T value;
    std::memcpy(&value, payload_.data(), sizeof(T));
    payload_ = payload_.subspan(sizeof(T));

    return value;
  }

  void ReadNode(OwnerNode &node) {
    node.ts = Read<timestamp_t>();
  }

  void ReadNode(WaiterNode &node) {
    node.ts = Read<timestamp_t>();
    node.worker_id = Read<wid_t>();
    node.mode = Read<LockMode>();
  }

  template <typename Node>
  auto ReadList() -> detail::TimestampSortedList<Node> {
    const auto count = Read<u16>();

    detail::TimestampSortedList<Node> list;
    for (u16 i = 0; i < count; i++) {
      Node node;
      ReadNode(node);
      list.Insert(std::move(node));
    }

    return list;
  }

 private:
  std::span<const u8> payload_;
};

} // namespace

auto WaitDieLockState::TryAcquire(LockParams params, bool holds_shared) -> LockResult {
  const auto txn_ts = params.txn_ts;
  const auto lock_mode =
    params.exclusive ? (holds_shared ? LockMode::UPGRADE : LockMode::EXCLUSIVE) : LockMode::SHARED;

  if (IsFree()) {
    Ensure(lock_mode != LockMode::UPGRADE);
    TakeOwnership(txn_ts, lock_mode);
    return LockResult::ACQUIRED;
  }

  if (lock_mode == LockMode::UPGRADE) {
    return UpgradeToExclusive(params);
  }
  
  if (owners_.Contains(txn_ts)) {
    return LockResult::ACQUIRED;
  }
  
  if (lock_mode == LockMode::SHARED && owners_mode_ == LockMode::SHARED && waiters_.IsEmpty()) {
    owners_.Insert(OwnerNode{txn_ts});
    return LockResult::ACQUIRED;
  }

  if (txn_ts < GetOldestOwnerTimestamp()) {
    Ensure(!waiters_.Contains(txn_ts));
    waiters_.Insert(WaiterNode{txn_ts, params.worker_id, lock_mode});
    return LockResult::MUST_WAIT;
  }

  return LockResult::ABORT;
}

auto WaitDieLockState::TakeOwnership(timestamp_t txn_ts, LockMode mode) -> void {
  Ensure(mode != LockMode::UPGRADE);
  owners_mode_ = mode;
  owners_.Insert(OwnerNode{txn_ts});
}

auto WaitDieLockState::GetOldestOwnerTimestamp() const -> timestamp_t {
  return owners_.Front().ts;
}

auto WaitDieLockState::UpgradeToExclusive(LockParams params) -> LockResult {
  const auto txn_ts = params.txn_ts;

  if (txn_ts != owners_.Front().ts) {
    return LockResult::ABORT;
  }

  if (owners_.Size() == 1) {
    owners_mode_ = LockMode::EXCLUSIVE;
    return LockResult::ACQUIRED;
  }

  waiters_.Insert(WaiterNode{txn_ts, params.worker_id, LockMode::UPGRADE});
  return LockResult::MUST_WAIT;
}

auto WaitDieLockState::PromoteWaiters() -> std::vector<wid_t> {
  std::vector<wid_t> workers_to_wake;

  size_t waiter_count = waiters_.Size();
  for (size_t i = 0; i < waiter_count; i++) {
    const auto &waiter = waiters_.Back();

    if (owners_mode_ == LockMode::NONE) {
      Ensure(waiter.mode != LockMode::UPGRADE);
      TakeOwnership(waiter.ts, waiter.mode);
    } else if (waiter.mode == LockMode::SHARED && owners_mode_ == LockMode::SHARED) {
      owners_.Insert(OwnerNode{waiter.ts});
    } else if (waiter.mode == LockMode::UPGRADE && owners_mode_ == LockMode::SHARED && owners_.Size() == 1) {
      Ensure(waiter.ts == owners_.Front().ts);
      owners_mode_ = LockMode::EXCLUSIVE;
    } else {
      Ensure((waiter.mode == LockMode::SHARED && owners_mode_ == LockMode::EXCLUSIVE) ||
             (waiter.mode == LockMode::EXCLUSIVE && owners_mode_ != LockMode::NONE) ||
             (waiter.mode == LockMode::UPGRADE && owners_mode_ == LockMode::SHARED && owners_.Size() > 1));
      break;
    }

    workers_to_wake.push_back(waiter.worker_id);
    waiters_.PopBack();
  }

  return workers_to_wake;
}

auto WaitDieLockState::ToRawBytes() const -> std::vector<u8> {
  ByteWriter writer(sizeof(LockMode) + COUNT_BYTES + owners_.Size() * OWNER_BYTES + COUNT_BYTES +
                    waiters_.Size() * WAITER_BYTES);

  writer.Write(owners_mode_);
  writer.WriteList(owners_);
  writer.WriteList(waiters_);

  return std::move(writer.payload);
}

auto WaitDieLockState::FromRawBytes(std::span<const u8> payload) -> WaitDieLockState {
  ByteReader reader(payload);

  WaitDieLockState state;
  state.owners_mode_ = reader.Read<LockMode>();
  state.owners_ = reader.ReadList<OwnerNode>();
  state.waiters_ = reader.ReadList<WaiterNode>();

  return state;
}

}  // namespace leanstore::transaction::svcc::inpage
