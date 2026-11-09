#include "buffer/buffer_manager.h"
#include "common/typedefs.h"
#include "test/base_test.h"
#include "transaction/svcc/evicted_lock_store.h"
#include "transaction/svcc/lock_state.h"
#include "transaction/svcc/page_lock_table.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace leanstore::transaction::svcc {

/**
 * SVCC in-page locking: the store of the locks of evicted pages, a tree of system pages.
 * The system pages have a budget of 1 MB (256 pages) here: large spills go to disk and come back
 */
class TestEvictedLockStore : public BaseTest {
 protected:
  /* The held locks of a page: key -> word of its LockState */
  using Locks = std::map<std::vector<u8>, u32>;

  static constexpr u32 OWNER_SHIFT = 16;

  void SetUp() override {
    BaseTest::SetupTestFile();
  }

  auto Store() -> EvictedLockStore & {
    return buffer_->EvictedLocks();
  }

  /* The key of the given length, unique per index */
  static auto Key(u32 index, size_t length) -> std::vector<u8> {
    std::vector<u8> key(length, 'x');
    for (size_t idx = 0; idx < sizeof(index); idx++) {
      key[idx] = static_cast<u8>(index >> (8 * (sizeof(index) - 1 - idx)));
    }
    return key;
  }

  static auto SharedWord(u32 holder_count) -> u32 {
    return holder_count;
  }

  static auto ExclusiveWord(wid_t worker_id) -> u32 {
    return static_cast<u32>(worker_id + 1) << OWNER_SHIFT;
  }

  /* count locks with keys of key_length bytes: shared ones with 1..3 holders and exclusive ones */
  static auto MakeLocks(u32 count, size_t key_length, u32 seed) -> Locks {
    std::mt19937 rng(seed);
    Locks locks;
    for (u32 index = 0; index < count; index++) {
      auto word = (rng() % 4 == 0) ? ExclusiveWord(rng() % 8) : SharedWord(1 + rng() % 3);
      locks.emplace(Key(index, key_length), word);
    }
    return locks;
  }

  void Spill(pageid_t pid, const Locks &locks) {
    PageLockTable table;
    for (const auto &[key, word] : locks) {
      table.AddLock(key, LockState{word});
    }
    Store().Spill(pid, table);
  }

  auto Restore(pageid_t pid) -> Locks {
    PageLockTable table;
    Store().Restore(pid, table);

    Locks locks;
    table.ForEachHeldLock([&](std::span<const u8> key, const LockState &lock) {
      locks.emplace(std::vector<u8>(key.begin(), key.end()), lock.word);
    });
    return locks;
  }

  /* Releases one holder of the lock of key, in the store and in the expected locks */
  void Release(pageid_t pid, Locks &locks, const std::vector<u8> &key) {
    auto &word = locks.at(key);
    const bool exclusive = (word >> OWNER_SHIFT) != 0;
    Store().Release(pid, key, exclusive);

    if (exclusive) {
      locks.erase(key);
    } else if (--word == 0) {
      locks.erase(key);
    }
  }
};

TEST_F(TestEvictedLockStore, RestoreReturnsSpilledLocks) {
  auto locks = MakeLocks(50, 16, 1);
  Spill(7, locks);

  EXPECT_EQ(Restore(7), locks);
  // The restore moved the locks out of the store
  EXPECT_TRUE(Restore(7).empty());
}

TEST_F(TestEvictedLockStore, RestoreOfPageWithoutLocks) {
  EXPECT_TRUE(Restore(7).empty());

  Spill(8, MakeLocks(10, 16, 1));
  EXPECT_TRUE(Restore(7).empty());
  EXPECT_TRUE(Restore(9).empty());
}

TEST_F(TestEvictedLockStore, PagesDoNotMix) {
  // Neighbors in the order of the store (big-endian pids), the same keys on every page
  const std::vector<pageid_t> pids = {1, 2, 255, 256, 257, 65536, 1ULL << 40};
  std::map<pageid_t, Locks> locks_of_page;
  for (auto pid : pids) {
    locks_of_page[pid] = MakeLocks(20, 12, static_cast<u32>(pid));
    Spill(pid, locks_of_page[pid]);
  }

  // Not in the order of the store
  for (auto pid : {256ULL, 1ULL, 1ULL << 40, 257ULL, 2ULL, 65536ULL, 255ULL}) {
    EXPECT_EQ(Restore(pid), locks_of_page[pid]) << "pid " << pid;
  }
}

TEST_F(TestEvictedLockStore, ReleaseWhileEvicted) {
  const auto shared_key = Key(1, 16);
  const auto exclusive_key = Key(2, 16);
  const auto single_shared_key = Key(3, 16);
  Locks locks = {
    {shared_key, SharedWord(3)},
    {exclusive_key, ExclusiveWord(2)},
    {single_shared_key, SharedWord(1)},
  };
  Spill(9, locks);

  Release(9, locks, shared_key);
  Release(9, locks, exclusive_key);
  Release(9, locks, single_shared_key);
  ASSERT_EQ(locks, (Locks{{shared_key, SharedWord(2)}}));

  EXPECT_EQ(Restore(9), locks);
}

TEST_F(TestEvictedLockStore, LocksOfPageOnManyLeaves) {
  // About 4000 * 212 bytes: hundreds of leaves, more than the 256 system pages in memory
  auto locks = MakeLocks(4000, 200, 2);
  auto left_locks = MakeLocks(30, 200, 3);
  auto right_locks = MakeLocks(30, 200, 4);
  Spill(41, left_locks);
  Spill(42, locks);
  Spill(43, right_locks);

  // A part of the locks leaves while the page is evicted
  for (u32 index = 0; index < 4000; index += 3) {
    Release(42, locks, Key(index, 200));
  }

  EXPECT_EQ(Restore(42), locks);
  // The neighbors are intact, also after the leaves emptied by the restore were merged
  EXPECT_EQ(Restore(41), left_locks);
  EXPECT_EQ(Restore(43), right_locks);
  EXPECT_TRUE(Restore(42).empty());

  // The store is usable again for the same page
  Spill(42, locks);
  EXPECT_EQ(Restore(42), locks);
}

TEST_F(TestEvictedLockStore, ConcurrentPages) {
  // The callers serialize the operations on one page: each thread has its own pages.
  // The pages of the threads interleave, i.e. the threads share the leaves of the store
  constexpr wid_t THREAD_COUNT = 4;
  constexpr pageid_t PAGES_PER_THREAD = 4;
  constexpr u32 LOCKS_PER_PAGE = 300;
  constexpr size_t KEY_LENGTH = 100;
  constexpr u32 ROUNDS = 10;

  std::vector<std::thread> threads;
  for (wid_t thread_id = 0; thread_id < THREAD_COUNT; thread_id++) {
    threads.emplace_back([&, thread_id]() {
      LeanStore::worker_thread_id = thread_id;
      std::mt19937 rng(thread_id);

      for (u32 round = 0; round < ROUNDS; round++) {
        std::map<pageid_t, Locks> locks_of_page;
        for (pageid_t idx = 0; idx < PAGES_PER_THREAD; idx++) {
          const pageid_t pid = 1000 + idx * THREAD_COUNT + thread_id;
          locks_of_page[pid] = MakeLocks(LOCKS_PER_PAGE, KEY_LENGTH, rng());
          Spill(pid, locks_of_page[pid]);
        }

        for (auto &[pid, locks] : locks_of_page) {
          for (u32 index = 0; index < LOCKS_PER_PAGE; index++) {
            auto key = Key(index, KEY_LENGTH);
            if (rng() % 2 == 0 && locks.contains(key)) {
              Release(pid, locks, key);
            }
          }
        }

        for (auto &[pid, locks] : locks_of_page) {
          EXPECT_EQ(Restore(pid), locks) << "pid " << pid << ", round " << round;
        }
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }
}

}  // namespace leanstore::transaction::svcc

auto main(int argc, char **argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);
  FLAGS_worker_count = 4;
  FLAGS_wal_enable = false;
  FLAGS_txn_mvcc = false;
  FLAGS_txn_lock_inpage = true;
  FLAGS_wal_enable_recovery = false;
  FLAGS_bm_system_physical_mb = 1;

  google::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
