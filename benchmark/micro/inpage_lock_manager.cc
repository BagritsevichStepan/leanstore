#include "leanstore/leanstore.h"
#include "transaction/lockable_tuple.h"
#include "transaction/svcc/inpage_lock_manager.h"
#include "transaction/svcc/page_lock_table.h"

#include <benchmark/benchmark.h>

#include <span>
#include <vector>

/**
 * Micro benchmarks of the transaction side of SVCC in-page locking, without B-tree:
 * the lock tables of the leaves are plain PageLockTables, a lock is released directly in the table of its leaf
 */

using leanstore::LeanStore;
using leanstore::transaction::LockableTuple;
using leanstore::transaction::svcc::InPageLockManager;
using leanstore::transaction::svcc::PageLockTable;

namespace {

static constexpr size_t KEY_LENGTH = 16;
static constexpr size_t NO_LEAVES = 16;

/* The keys of a transaction, spread over the lock tables of NO_LEAVES leaves */
class Workload {
 public:
  explicit Workload(size_t count) : tables_(NO_LEAVES) {
    for (size_t idx = 0; idx < count; idx++) {
      std::vector<u8> key(KEY_LENGTH, 0);
      for (size_t byte = 0; byte < 8; byte++) {
        key[KEY_LENGTH - 1 - byte] = static_cast<u8>((idx >> (8 * byte)) & 0xFF);
      }
      tuples_.push_back(LockableTuple::Constructor(std::span<u8>(key), 0));
    }
  }

  ~Workload() {
    for (auto tuple : tuples_) {
      LockableTuple::Release(tuple);
    }
  }

  Workload(const Workload &) = delete;
  auto operator=(const Workload &) -> Workload & = delete;

  auto Tuple(size_t idx) -> const LockableTuple * {
    return tuples_[idx];
  }

  static auto Leaf(size_t idx) -> pageid_t {
    return idx % NO_LEAVES;
  }

  auto Table(size_t idx) -> PageLockTable & {
    return tables_[Leaf(idx)];
  }

  void ReleaseAll(InPageLockManager &manager) {
    manager.ReleaseAllLocks([](const LockableTuple *, std::span<const u8>) {},
                            [&](const LockableTuple *tuple, pageid_t leaf, bool exclusive) {
                              tables_[leaf].Release({tuple->key, tuple->key_len}, exclusive);
                            });
  }

 private:
  std::vector<PageLockTable> tables_;
  std::vector<LockableTuple *> tuples_;
};

/**
 * One transaction: `reads` shared locks and `writes` exclusive locks (with an undo payload of `payload` bytes)
 * on distinct keys, then the commit releases them all. Profiles like TPC-C: Payment 5/3, NewOrder 20/20,
 * StockLevel 400/0
 */
void Txn(benchmark::State &state) {
  auto reads = static_cast<size_t>(state.range(0));
  auto writes = static_cast<size_t>(state.range(1));
  std::vector<u8> undo(state.range(2), 0);
  LeanStore::worker_thread_id = static_cast<wid_t>(state.thread_index());

  InPageLockManager manager;
  Workload workload(reads + writes);
  for (auto _ : state) {
    for (size_t idx = 0; idx < reads; idx++) {
      benchmark::DoNotOptimize(manager.TryLockShared(workload.Tuple(idx), workload.Table(idx), Workload::Leaf(idx)));
    }
    for (size_t idx = reads; idx < reads + writes; idx++) {
      benchmark::DoNotOptimize(
        manager.TryLock(std::span<u8>(undo), workload.Tuple(idx), workload.Table(idx), Workload::Leaf(idx)));
    }
    workload.ReleaseAll(manager);
  }
  state.SetItemsProcessed(state.iterations() * (reads + writes));
}
BENCHMARK(Txn)->Args({5, 3, 100})->Args({20, 20, 100})->Args({400, 0, 0});
BENCHMARK(Txn)->Args({20, 20, 100})->Threads(1)->Threads(4)->Threads(8)->UseRealTime();

/* A key locked again within the transaction: the lookup in the transaction's own sets only */
void RepeatedKey(benchmark::State &state) {
  LeanStore::worker_thread_id = 0;
  InPageLockManager manager;
  Workload workload(1);
  std::vector<u8> undo(100, 0);
  manager.TryLock(std::span<u8>(undo), workload.Tuple(0), workload.Table(0), Workload::Leaf(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(manager.TryLockShared(workload.Tuple(0), workload.Table(0), Workload::Leaf(0)));
  }
  state.SetItemsProcessed(state.iterations());
  workload.ReleaseAll(manager);
}
BENCHMARK(RepeatedKey);

}  // namespace

BENCHMARK_MAIN();
