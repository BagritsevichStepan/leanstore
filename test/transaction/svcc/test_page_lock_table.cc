#include "common/typedefs.h"
#include "transaction/svcc/page_lock_state.h"
#include "transaction/svcc/page_lock_table.h"

#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace leanstore;
using namespace leanstore::transaction::svcc;

static const ComparisonLambda CMP = {ComparisonOperator::MEMCMP, [](const void *a, const void *b, size_t len) {
                                       return std::memcmp(a, b, len);
                                     }};

using Key = std::string;

static auto AcquireShared(PageLockTable &table, const Key &key, wid_t worker_id) -> bool {
  return table.TryAcquire(key, worker_id, false);
}

static auto AcquireExclusive(PageLockTable &table, const Key &key, wid_t worker_id, bool holds_shared) -> bool {
  return table.TryAcquire(key, worker_id, true, holds_shared);
}

static void ReleaseShared(PageLockTable &table, const Key &key) {
  table.Release(key, false);
}

static void ReleaseExclusive(PageLockTable &table, const Key &key) {
  table.Release(key, true);
}

TEST(LockStateTest, SharedLocksAreCompatible) {
  LockState state;
  EXPECT_TRUE(state.IsFree());
  EXPECT_TRUE(state.TryAcquireShared(1));
  EXPECT_TRUE(state.TryAcquireShared(2));
  EXPECT_FALSE(state.TryAcquireExclusive(3, false));
  // Can't upgrade while another holder exists
  EXPECT_FALSE(state.TryAcquireExclusive(1, true));
  state.ReleaseShared();
  EXPECT_TRUE(state.TryAcquireExclusive(1, true));
  state.ReleaseExclusive();
  EXPECT_TRUE(state.IsFree());
}

TEST(LockStateTest, ExclusiveBlocksEveryoneElse) {
  LockState state;
  EXPECT_TRUE(state.TryAcquireExclusive(1, false));
  EXPECT_TRUE(state.TryAcquireExclusive(1, false));  // re-entrant
  EXPECT_FALSE(state.TryAcquireShared(2));
  EXPECT_FALSE(state.TryAcquireExclusive(2, false));
  state.ReleaseExclusive();
  EXPECT_TRUE(state.TryAcquireShared(2));
  state.ReleaseShared();
  EXPECT_TRUE(state.TryAcquireExclusive(2, false));
}

TEST(PageLockTableTest, OnlyHeldLocksStay) {
  PageLockTable table;
  Key k1 = {1, 2, 3};
  Key k2 = {1, 2, 4};

  EXPECT_TRUE(AcquireShared(table, k1, 1));
  EXPECT_TRUE(AcquireShared(table, k1, 2));
  EXPECT_TRUE(AcquireExclusive(table, k2, 3, false));

  // A failed acquisition of an existing lock leaves it as it is
  EXPECT_FALSE(AcquireExclusive(table, k1, 4, false));
  EXPECT_FALSE(AcquireShared(table, k2, 4));

  ReleaseShared(table, k1);
  ReleaseShared(table, k1);
  EXPECT_FALSE(table.Empty());
  ReleaseExclusive(table, k2);
  EXPECT_TRUE(table.Empty());
}

TEST(PageLockTableTest, Upgrade) {
  PageLockTable table;
  Key k = {5};
  EXPECT_TRUE(AcquireShared(table, k, 1));
  EXPECT_TRUE(AcquireExclusive(table, k, 1, true));
  EXPECT_FALSE(AcquireShared(table, k, 2));
  ReleaseExclusive(table, k);
  EXPECT_TRUE(table.Empty());
}

TEST(PageLockTableTest, SplitMovesUpperKeys) {
  PageLockTable left;
  PageLockTable right;

  std::vector<Key> keys = {{1}, {2}, {3}, {3, 0}, {4}};
  for (auto &k : keys) {
    ASSERT_TRUE(AcquireShared(left, k, 1));
  }
  ASSERT_TRUE(AcquireExclusive(left, keys[4], 1, true));

  left.MoveUpperRange(Key{3}, CMP, right);

  // keys > separator moved with their state: they are held in `right` now
  EXPECT_FALSE(AcquireShared(right, keys[4], 2));
  ReleaseExclusive(right, keys[4]);
  ReleaseShared(right, keys[3]);
  EXPECT_TRUE(right.Empty());
  // keys <= separator stayed
  for (auto idx = 0; idx < 3; idx++) {
    ReleaseShared(left, keys[idx]);
  }
  EXPECT_TRUE(left.Empty());
}

TEST(PageLockTableTest, MergeMovesEverything) {
  PageLockTable left;
  PageLockTable right;
  Key l = {2};
  Key r = {8};
  ASSERT_TRUE(AcquireExclusive(left, l, 1, false));
  ASSERT_TRUE(AcquireShared(right, r, 2));

  left.MergeInto(right);

  EXPECT_TRUE(left.Empty());
  EXPECT_FALSE(AcquireShared(right, l, 3));
  ReleaseExclusive(right, l);
  ReleaseShared(right, r);
  EXPECT_TRUE(right.Empty());
}

TEST(PageLockStateTest, ConcurrentSharedLocks) {
  static constexpr int NO_THREADS = 8;
  static constexpr int NO_KEYS = 1000;
  PageLockState page;

  std::vector<std::thread> threads;
  for (int t = 0; t < NO_THREADS; t++) {
    threads.emplace_back([&, t]() {
      for (int k = 0; k < NO_KEYS; k++) {
        Key key(sizeof(int), 0);
        std::memcpy(key.data(), &k, sizeof(int));
        // The table itself is not thread-safe: every access goes through the latch of its PageLockState
        auto guard = page.Lock();
        EXPECT_TRUE(AcquireShared(guard.GetLockTable(), key, t));
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  // Every thread holds a shared lock on every key: an exclusive one can't be granted
  auto guard = page.Lock();
  for (int k = 0; k < NO_KEYS; k++) {
    Key key(sizeof(int), 0);
    std::memcpy(key.data(), &k, sizeof(int));
    EXPECT_FALSE(AcquireExclusive(guard.GetLockTable(), key, NO_THREADS + 1, false));
  }
}
