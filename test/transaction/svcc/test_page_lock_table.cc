#include "buffer/buffer_frame.h"
#include "common/typedefs.h"
#include "transaction/svcc/page_lock_table.h"

#include "gtest/gtest.h"

#include <cstring>
#include <set>
#include <thread>
#include <vector>

using namespace leanstore;
using namespace leanstore::transaction::svcc;

static const ComparisonLambda CMP = {ComparisonOperator::MEMCMP, [](const void *a, const void *b, size_t len) {
                                       return std::memcmp(a, b, len);
                                     }};

static auto Key(std::vector<u8> &&bytes) -> std::vector<u8> { return std::move(bytes); }

TEST(LockStateTest, SharedLocksAreCompatible) {
  LockState state;
  EXPECT_TRUE(state.TryAcquireShared(1));
  EXPECT_TRUE(state.TryAcquireShared(2));
  EXPECT_FALSE(state.TryAcquireExclusive(3, false));
  // Can't upgrade while another holder exists
  EXPECT_FALSE(state.TryAcquireExclusive(1, true));
  state.ReleaseShared();
  EXPECT_TRUE(state.TryAcquireExclusive(1, true));
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

TEST(PageLockTableTest, OneStatePerKey) {
  buffer::BufferFrame frame;
  frame.Init();
  EXPECT_EQ(frame.GetLockTable(), nullptr);
  auto table = frame.GetOrCreateLockTable();
  EXPECT_EQ(frame.GetOrCreateLockTable(), table);

  auto k1 = Key({1, 2, 3});
  auto k2 = Key({1, 2, 4});
  EXPECT_EQ(table->GetOrCreate(k1), table->GetOrCreate(k1));
  EXPECT_NE(table->GetOrCreate(k1), table->GetOrCreate(k2));
}

TEST(PageLockTableTest, SplitMovesUpperKeysAndKeepsAddresses) {
  buffer::BufferFrame left_frame;
  buffer::BufferFrame right_frame;
  left_frame.Init();
  right_frame.Init();
  auto left  = left_frame.GetOrCreateLockTable();
  auto right = right_frame.GetOrCreateLockTable();

  std::vector<std::vector<u8>> keys = {{1}, {2}, {3}, {3, 0}, {4}};
  std::vector<LockState *> states;
  for (auto &k : keys) { states.push_back(left->GetOrCreate(k)); }
  ASSERT_TRUE(states[4]->TryAcquireExclusive(7, false));

  auto sep = Key({3});
  left->MoveUpperRange(sep, CMP, *right);

  // keys <= sep stay, keys > sep move -- as the very same objects
  EXPECT_EQ(left->GetOrCreate(keys[0]), states[0]);
  EXPECT_EQ(left->GetOrCreate(keys[2]), states[2]);
  EXPECT_EQ(right->GetOrCreate(keys[3]), states[3]);
  EXPECT_EQ(right->GetOrCreate(keys[4]), states[4]);
  // The lock moved with its key
  EXPECT_FALSE(right->GetOrCreate(keys[4])->TryAcquireShared(8));
  // The left table forgot the moved keys
  EXPECT_NE(left->GetOrCreate(keys[4]), states[4]);
}

TEST(PageLockTableTest, MergeKeepsTheEntryOfTheCoveringLeaf) {
  buffer::BufferFrame left_frame;
  buffer::BufferFrame right_frame;
  left_frame.Init();
  right_frame.Init();
  auto left  = left_frame.GetOrCreateLockTable();
  auto right = right_frame.GetOrCreateLockTable();

  auto left_fence = Key({5});  // left covers <= 5, right covers > 5
  auto own_left   = Key({2});  // real in left, stale duplicate in right
  auto own_right  = Key({8});  // stale duplicate in left, real in right
  auto only_left  = Key({3});

  auto real_left  = left->GetOrCreate(own_left);
  auto stale_left = left->GetOrCreate(own_right);
  auto plain_left = left->GetOrCreate(only_left);
  auto stale_right = right->GetOrCreate(own_left);
  auto real_right  = right->GetOrCreate(own_right);
  ASSERT_TRUE(real_left->TryAcquireExclusive(1, false));
  ASSERT_TRUE(real_right->TryAcquireExclusive(2, false));

  left->MergeInto(*right, left_fence, CMP);

  EXPECT_EQ(right->GetOrCreate(own_left), real_left);
  EXPECT_EQ(right->GetOrCreate(own_right), real_right);
  EXPECT_EQ(right->GetOrCreate(only_left), plain_left);
  EXPECT_NE(right->GetOrCreate(own_left), stale_right);
  EXPECT_NE(right->GetOrCreate(own_right), stale_left);
  // Buried states stay valid: their holders still release them through the pointer
  EXPECT_TRUE(stale_left->TryAcquireShared(3));
  stale_left->ReleaseShared();
}

TEST(PageLockTableTest, ConcurrentGetOrCreateYieldsOneState) {
  static constexpr int NO_THREADS = 8;
  static constexpr int NO_KEYS    = 1000;
  buffer::BufferFrame frame;
  frame.Init();

  std::vector<std::vector<LockState *>> seen(NO_THREADS, std::vector<LockState *>(NO_KEYS));
  std::vector<std::thread> threads;
  for (int t = 0; t < NO_THREADS; t++) {
    threads.emplace_back([&, t]() {
      auto table = frame.GetOrCreateLockTable();
      for (int k = 0; k < NO_KEYS; k++) {
        u8 key[sizeof(int)];
        std::memcpy(key, &k, sizeof(int));
        seen[t][k] = table->GetOrCreate(key);
        EXPECT_TRUE(seen[t][k]->TryAcquireShared(t + 1));
      }
    });
  }
  for (auto &th : threads) { th.join(); }

  std::set<LockState *> distinct;
  for (int k = 0; k < NO_KEYS; k++) {
    for (int t = 1; t < NO_THREADS; t++) { EXPECT_EQ(seen[t][k], seen[0][k]); }
    distinct.insert(seen[0][k]);
    // every thread holds a shared lock, an exclusive one can't be granted
    EXPECT_FALSE(seen[0][k]->TryAcquireExclusive(NO_THREADS + 1, false));
  }
  EXPECT_EQ(distinct.size(), static_cast<size_t>(NO_KEYS));
}
