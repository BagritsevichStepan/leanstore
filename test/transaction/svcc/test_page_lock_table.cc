#include "common/typedefs.h"
#include "transaction/svcc/page_lock_state.h"
#include "transaction/svcc/page_lock_table.h"

#include "gtest/gtest.h"

#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace leanstore;
using namespace leanstore::transaction::svcc;

static const ComparisonLambda CMP = {ComparisonOperator::MEMCMP, [](const void *a, const void *b, size_t len) {
                                       return std::memcmp(a, b, len);
                                     }};

static const ComparisonLambda DESCENDING_CMP = {ComparisonOperator::BLOB_HANDLER,
                                                [](const void *a, const void *b, size_t len) {
                                                  return std::memcmp(b, a, len);
                                                }};

using Key = std::vector<u8>;

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
  EXPECT_TRUE(state.TryAcquire(1, false));
  EXPECT_TRUE(state.TryAcquire(2, false));
  EXPECT_FALSE(state.TryAcquire(3, true, false));
  // Can't upgrade while another holder exists
  EXPECT_FALSE(state.TryAcquire(1, true, true));
  state.Release(false);
  EXPECT_TRUE(state.TryAcquire(1, true, true));
  state.Release(true);
  EXPECT_TRUE(state.IsFree());
}

TEST(LockStateTest, ExclusiveBlocksEveryoneElse) {
  LockState state;
  EXPECT_TRUE(state.TryAcquire(1, true, false));
  EXPECT_TRUE(state.TryAcquire(1, true, false));  // re-entrant
  EXPECT_FALSE(state.TryAcquire(2, false));
  EXPECT_FALSE(state.TryAcquire(2, true, false));
  state.Release(true);
  EXPECT_TRUE(state.TryAcquire(2, false));
  state.Release(false);
  EXPECT_TRUE(state.TryAcquire(2, true, false));
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
  EXPECT_FALSE(table.IsEmpty());
  ReleaseExclusive(table, k2);
  EXPECT_TRUE(table.IsEmpty());
}

TEST(PageLockTableTest, Upgrade) {
  PageLockTable table;
  Key k = {5};
  EXPECT_TRUE(AcquireShared(table, k, 1));
  EXPECT_TRUE(AcquireExclusive(table, k, 1, true));
  EXPECT_FALSE(AcquireShared(table, k, 2));
  ReleaseExclusive(table, k);
  EXPECT_TRUE(table.IsEmpty());
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
  EXPECT_TRUE(right.IsEmpty());
  // keys <= separator stayed
  for (auto idx = 0; idx < 3; idx++) {
    ReleaseShared(left, keys[idx]);
  }
  EXPECT_TRUE(left.IsEmpty());
}

TEST(PageLockTableTest, MergeMovesEverything) {
  PageLockTable left;
  PageLockTable right;
  Key l = {2};
  Key r = {8};
  ASSERT_TRUE(AcquireExclusive(left, l, 1, false));
  ASSERT_TRUE(AcquireShared(right, r, 2));

  left.MergeInto(right);

  EXPECT_TRUE(left.IsEmpty());
  EXPECT_FALSE(AcquireShared(right, l, 3));
  ReleaseExclusive(right, l);
  ReleaseShared(right, r);
  EXPECT_TRUE(right.IsEmpty());
}

TEST(PageLockTableTest, ReleasedKeyCanBeLockedAgain) {
  PageLockTable table;
  Key k = {7};
  ASSERT_TRUE(AcquireExclusive(table, k, 1, false));
  ReleaseExclusive(table, k);
  EXPECT_TRUE(table.IsEmpty());

  // The entry of the key stays with a free lock: it behaves like a new one
  EXPECT_TRUE(AcquireShared(table, k, 2));
  EXPECT_FALSE(table.IsEmpty());
  EXPECT_FALSE(AcquireExclusive(table, k, 3, false));
  ReleaseShared(table, k);
  EXPECT_TRUE(table.IsEmpty());
  EXPECT_TRUE(AcquireExclusive(table, k, 3, false));
  ReleaseExclusive(table, k);
  EXPECT_TRUE(table.IsEmpty());
}

TEST(PageLockTableTest, TryReleaseOfLockNotHeld) {
  PageLockTable table;
  Key k = {1, 2};
  EXPECT_FALSE(table.TryRelease(k, false));
  EXPECT_FALSE(table.TryRelease(k, true));

  ASSERT_TRUE(AcquireShared(table, k, 1));

  // A prefix or an extension of a key is another key
  EXPECT_FALSE(table.TryRelease(Key{1}, false));
  EXPECT_FALSE(table.TryRelease(Key{1, 2, 0}, false));
  EXPECT_TRUE(table.TryRelease(k, false));
  EXPECT_TRUE(table.IsEmpty());
}

TEST(PageLockTableTest, PrefixesAndLongKeys) {
  PageLockTable table;
  const std::vector<Key> keys = {Key(1, 0), Key(2, 0), Key(300, 'a'), Key(301, 'a'), Key(300, 'b')};

  // Keys of more than 15 bytes are not inline in std::string
  for (auto idx = keys.size(); idx-- > 0;) {
    ASSERT_TRUE(AcquireExclusive(table, keys[idx], static_cast<wid_t>(idx + 1), false));
  }

  for (auto &key : keys) {
    EXPECT_FALSE(AcquireShared(table, key, 100));
  }

  for (auto &key : keys) {
    EXPECT_FALSE(table.IsEmpty());
    ReleaseExclusive(table, key);
  }

  EXPECT_TRUE(table.IsEmpty());
}

TEST(PageLockTableTest, CompactKeepsHeldLocks) {
  static constexpr int no_keys = 200;
  static constexpr int held_every = 20;

  PageLockTable table;

  auto key_of = [](int idx) {
    return Key{static_cast<u8>(idx / 256), static_cast<u8>(idx % 256)};
  };

  auto is_held = [](int idx) {
    return idx % held_every == 0;
  };
  
  auto check = [&]() {
    for (int idx = 0; idx < no_keys; idx++) {
      auto key = key_of(idx);

      if (is_held(idx)) {
        EXPECT_FALSE(AcquireShared(table, key, 3));
        continue;
      }
      
      EXPECT_TRUE(AcquireExclusive(table, key, 3, false));
      ReleaseExclusive(table, key);
    }
  };

  for (int idx = 0; idx < no_keys; idx++) {
    if (is_held(idx)) {
      ASSERT_TRUE(AcquireExclusive(table, key_of(idx), 1, false));
    } else {
      ASSERT_TRUE(AcquireShared(table, key_of(idx), 2));
    }
  }

  // More than MAX_FREE_ENTRIES releases: the free entries are erased on the way
  for (int idx = 0; idx < no_keys; idx++) {
    if (!is_held(idx)) {
      ReleaseShared(table, key_of(idx));
    }
  }

  EXPECT_FALSE(table.IsEmpty());
  check();

  table.Compact();
  check();

  for (int idx = 0; idx < no_keys; idx += held_every) {
    ReleaseExclusive(table, key_of(idx));
  }

  EXPECT_TRUE(table.IsEmpty());
  table.Compact();
  EXPECT_TRUE(table.IsEmpty());
}

TEST(PageLockTableTest, SplitMovesOnlyHeldLocks) {
  PageLockTable left;
  PageLockTable right;
  Key low = {1};
  Key high = {9};

  ASSERT_TRUE(AcquireShared(left, low, 1));
  ASSERT_TRUE(AcquireShared(left, high, 1));
  ReleaseShared(left, high);

  left.MoveUpperRange(Key{5}, CMP, right);

  // The lock of `high` was released before the split: nothing is handed over
  EXPECT_TRUE(right.IsEmpty());
  EXPECT_FALSE(right.TryRelease(high, false));
  EXPECT_FALSE(left.TryRelease(high, false));
  ReleaseShared(left, low);
  EXPECT_TRUE(left.IsEmpty());
}

TEST(PageLockTableTest, MergeKeepsEntriesOfRight) {
  PageLockTable left;
  PageLockTable right;

  const std::vector<Key> left_keys = {{1}, {2, 0}, {3}};
  const std::vector<Key> right_held = {{5}, {7}};
  const Key right_free = {6};

  for (auto &key : left_keys) {
    ASSERT_TRUE(AcquireExclusive(left, key, 1, false));
  }

  ASSERT_TRUE(AcquireShared(left, Key{2}, 1));
  ReleaseShared(left, Key{2});
  for (auto &key : right_held) {
    ASSERT_TRUE(AcquireShared(right, key, 2));
  }
  ASSERT_TRUE(AcquireShared(right, right_free, 2));
  ReleaseShared(right, right_free);

  left.MergeInto(right);

  EXPECT_TRUE(left.IsEmpty());
  EXPECT_FALSE(right.TryRelease(Key{2}, false));
  // The lock of right_free was released before the merge: it is free
  EXPECT_TRUE(AcquireExclusive(right, right_free, 3, false));
  ReleaseExclusive(right, right_free);
  for (auto &key : left_keys) {
    EXPECT_FALSE(left.TryRelease(key, true));
    EXPECT_FALSE(AcquireShared(right, key, 3));
    ReleaseExclusive(right, key);
  }

  for (auto &key : right_held) {
    EXPECT_FALSE(AcquireExclusive(right, key, 3, false));
    ReleaseShared(right, key);
  }

  EXPECT_TRUE(right.IsEmpty());
}

TEST(PageLockTableTest, SplitWithDescendingOrder) {
  PageLockTable left;
  PageLockTable right;

  const std::vector<Key> staying = {{0xFF}, {0xC0}, {0x80}};
  const std::vector<Key> moving = {{0x80, 0x00}, {0x40}, {0x00}};

  for (auto &key : staying) {
    ASSERT_TRUE(AcquireShared(left, key, 1));
  }

  for (auto &key : moving) {
    ASSERT_TRUE(AcquireExclusive(left, key, 1, false));
  }

  // DESCENDING_CMP, not the byte order: {0xFF} < {0xC0} < {0x80} = separator < {0x80, 0x00} < {0x40} < {0x00}
  left.MoveUpperRange(Key{0x80}, DESCENDING_CMP, right);

  for (auto &key : staying) {
    EXPECT_FALSE(right.TryRelease(key, false));
    ReleaseShared(left, key);
  }

  for (auto &key : moving) {
    EXPECT_FALSE(left.TryRelease(key, true));
    ReleaseExclusive(right, key);
  }

  EXPECT_TRUE(left.IsEmpty());
  EXPECT_TRUE(right.IsEmpty());
}

TEST(PageLockTableTest, MergeWithDescendingOrder) {
  PageLockTable left;
  PageLockTable right;

  const std::vector<Key> left_keys = {{0xFF}, {0xC0}, {0x80}};
  const std::vector<Key> right_keys = {{0x80, 0x00}, {0x40}, {0x00}};

  for (auto &key : left_keys) {
    ASSERT_TRUE(AcquireShared(left, key, 1));
  }
  for (auto &key : right_keys) {
    ASSERT_TRUE(AcquireExclusive(right, key, 1, false));
  }

  // DESCENDING_CMP: every key of the left leaf is below every key of the right one
  left.MergeInto(right);

  EXPECT_TRUE(left.IsEmpty());

  for (auto &key : left_keys) {
    EXPECT_FALSE(AcquireExclusive(right, key, 2, false));
    ReleaseShared(right, key);
  }

  for (auto &key : right_keys) {
    EXPECT_FALSE(AcquireShared(right, key, 2));
    ReleaseExclusive(right, key);
  }
  
  EXPECT_TRUE(right.IsEmpty());
}

TEST(PageLockTableTest, SplitOrdersLongKeysAsMemcmp) {
  // The first differing byte decides, also when a later byte differs the other way; bytes are unsigned
  auto key_with = [](std::initializer_list<std::pair<size_t, u8>> bytes) {
    Key key(12, 0x11);
    for (auto [pos, value] : bytes) {
      key[pos] = value;
    }
    return key;
  };
  const Key separator = key_with({});
  const std::vector<Key> staying = {
    separator,
    key_with({{0, 0x10}, {7, 0xFF}}),
    key_with({{6, 0x01}}),
    key_with({{9, 0x00}}),
    Key(5, 0x11),
  };
  const std::vector<Key> moving = {
    key_with({{0, 0x12}, {7, 0x00}}),
    key_with({{6, 0x80}}),
    key_with({{11, 0x12}}),
    Key(13, 0x11),
  };

  PageLockTable left;
  PageLockTable right;
  for (auto &key : staying) {
    ASSERT_TRUE(AcquireShared(left, key, 1));
  }
  for (auto &key : moving) {
    ASSERT_TRUE(AcquireExclusive(left, key, 1, false));
  }

  left.MoveUpperRange(separator, CMP, right);

  for (auto &key : staying) {
    EXPECT_FALSE(right.TryRelease(key, false));
    ReleaseShared(left, key);
  }
  for (auto &key : moving) {
    EXPECT_FALSE(left.TryRelease(key, true));
    ReleaseExclusive(right, key);
  }
  EXPECT_TRUE(left.IsEmpty());
  EXPECT_TRUE(right.IsEmpty());
}
