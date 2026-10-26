#include "leanstore/kv_interface.h"
#include "transaction/svcc/page_lock_state.h"
#include "transaction/svcc/page_lock_table.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

/**
 * Micro benchmarks of the per-leaf lock table of SVCC in-page locking (single-threaded unless stated otherwise).
 * Arguments: key count and key length in bytes (8, 16 and 32: std::string keeps up to 15 bytes inline)
 */

using leanstore::ComparisonLambda;
using leanstore::ComparisonOperator;
using leanstore::transaction::svcc::PageLockState;
using leanstore::transaction::svcc::PageLockTable;

namespace {

const ComparisonLambda CMP = {ComparisonOperator::MEMCMP, [](const void *lhs, const void *rhs, size_t len) {
                                return std::memcmp(lhs, rhs, len);
                              }};

/* `count` distinct keys of `length` bytes, increasing in memcmp order */
auto MakeKeys(size_t count, size_t length) -> std::vector<std::vector<u8>> {
  std::vector<std::vector<u8>> keys;
  keys.reserve(count);
  for (size_t idx = 0; idx < count; idx++) {
    std::vector<u8> key(length, 0);
    for (size_t byte = 0; byte < std::min<size_t>(length, 8); byte++) {
      key[length - 1 - byte] = static_cast<u8>((idx >> (8 * byte)) & 0xFF);
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

/* Shared locks on N new keys, then released: every lock creates and erases its entry */
void NewKeys(benchmark::State &state) {
  auto keys = MakeKeys(state.range(0), state.range(1));
  PageLockTable table;
  for (auto _ : state) {
    for (auto &key : keys) {
      benchmark::DoNotOptimize(table.TryAcquire(key, 1, false));
    }
    for (auto &key : keys) {
      table.Release(key, false);
    }
  }
  state.SetItemsProcessed(state.iterations() * keys.size());
}

BENCHMARK(NewKeys)->ArgsProduct({{1, 8, 32, 128}, {8, 16, 32}});

/* The same key locked and released over and over, e.g. a district */
void HotKey(benchmark::State &state) {
  auto keys = MakeKeys(1, state.range(0));
  PageLockTable table;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.TryAcquire(keys[0], 1, true));
    table.Release(keys[0], true);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(HotKey)->Arg(8)->Arg(16)->Arg(32);

/* M locks held by others, a shared lock on one of these existing keys: the lookup cost by table size */
void ExistingKey(benchmark::State &state) {
  auto keys = MakeKeys(state.range(0), state.range(1));
  PageLockTable table;
  for (auto &key : keys) {
    table.TryAcquire(key, 2, false);
  }
  auto &key = keys[keys.size() / 2];
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.TryAcquire(key, 1, false));
    table.Release(key, false);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(ExistingKey)->ArgsProduct({{1, 8, 32, 128, 512}, {16}});

/**
 * A leaf that many distinct keys pass through, e.g. new orders: M locks are held, every iteration locks a new key and
 * releases the oldest one. Erased entries are not reused by the next key
 */
void SlidingKeys(benchmark::State &state) {
  static constexpr size_t NO_KEYS = 1 << 16;
  auto held = static_cast<size_t>(state.range(0));
  auto keys = MakeKeys(NO_KEYS, 16);
  PageLockTable table;
  std::deque<size_t> window;
  size_t next = 0;
  for (; next < held; next++) {
    table.TryAcquire(keys[next], 1, true);
    window.push_back(next);
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.TryAcquire(keys[next % NO_KEYS], 1, true));
    window.push_back(next % NO_KEYS);
    next++;
    table.Release(keys[window.front()], true);
    window.pop_front();
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(SlidingKeys)->Arg(1)->Arg(8)->Arg(32)->Arg(128);

/* Shared lock, upgrade to exclusive, release */
void Upgrade(benchmark::State &state) {
  auto keys = MakeKeys(1, state.range(0));
  PageLockTable table;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.TryAcquire(keys[0], 1, false));
    benchmark::DoNotOptimize(table.TryAcquire(keys[0], 1, true, true));
    table.Release(keys[0], true);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(Upgrade)->Arg(16);

/* Leaf split: the upper half of M locks moves to the new right leaf (the table is refilled outside the timing) */
void MoveUpperRange(benchmark::State &state) {
  auto keys = MakeKeys(state.range(0), 16);
  auto &separator = keys[keys.size() / 2];
  for (auto _ : state) {
    state.PauseTiming();
    PageLockTable left;
    PageLockTable right;
    for (auto &key : keys) {
      left.TryAcquire(key, 1, false);
    }
    state.ResumeTiming();
    left.MoveUpperRange(separator, CMP, right);
    benchmark::DoNotOptimize(right);
  }
  state.SetItemsProcessed(state.iterations() * keys.size());
}

BENCHMARK(MoveUpperRange)->Arg(8)->Arg(32)->Arg(128);

/* Leaf merge: all M locks of the left leaf move to the right one */
void MergeInto(benchmark::State &state) {
  auto keys = MakeKeys(state.range(0), 16);
  for (auto _ : state) {
    state.PauseTiming();
    PageLockTable left;
    PageLockTable right;
    for (auto &key : keys) {
      left.TryAcquire(key, 1, false);
    }
    state.ResumeTiming();
    left.MergeInto(right);
    benchmark::DoNotOptimize(right);
  }
  state.SetItemsProcessed(state.iterations() * keys.size());
}

BENCHMARK(MergeInto)->Arg(8)->Arg(32)->Arg(128);

/* All threads lock the same key of the same leaf through its lock state latch: the latch under contention */
void LockedHotKey(benchmark::State &state) {
  static PageLockState page;
  auto keys = MakeKeys(1, 16);
  auto worker_id = static_cast<wid_t>(state.thread_index());
  for (auto _ : state) {
    {
      auto guard = page.Lock();
      benchmark::DoNotOptimize(guard.GetOrCreateLockTable().TryAcquire(keys[0], worker_id, false));
    }
    {
      auto guard = page.Lock();
      guard.GetOrCreateLockTable().Release(keys[0], false);
    }
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(LockedHotKey)->Threads(1)->Threads(2)->Threads(4)->Threads(8)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
