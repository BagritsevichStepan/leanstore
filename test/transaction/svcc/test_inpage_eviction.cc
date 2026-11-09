#include "common/typedefs.h"
#include "storage/btree/node.h"
#include "sync/page_state.h"
#include "test/base_test.h"
#include "transaction/svcc/page_lock_state.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <functional>
#include <thread>

namespace leanstore {

using ::testing::ElementsAreArray;
using transaction::IsolationLevel;
using transaction::svcc::PageLockState;

/**
 * SVCC in-page locking: the locks of an evicted leaf are spilled to the EvictedLockStore
 * and come back into the leaf's table when it is loaded again.
 *
 * The test thread is worker 0, a second transaction runs in its own thread as worker 1.
 * WAL is off, i.e. dirty pages are evicted without waiting for the log
 */
class TestSVCCInPageEviction : public BaseTest {
 protected:
  static constexpr wid_t OTHER_WORKER = 1;
  // Large enough to fit only a few dozen records into a leaf
  static constexpr size_t LARGE_VALUE = 150;

  std::unique_ptr<storage::BTree> tree_;

  void SetUp() override {
    BaseTest::SetupTestFile();
    InitRandTransaction();
    tree_ = std::make_unique<storage::BTree>(buffer_.get(), 0);
    catalog.push_back(tree_.get());
    txn_man_->CommitTransaction();
  }

  void TearDown() override {
    tree_.reset();
    BaseTest::TearDown();
  }

  static auto Key(u8 key) -> std::vector<u8> {
    return {key};
  }

  static auto Value(u8 key, size_t size) -> std::vector<u8> {
    return std::vector<u8>(size, key);
  }

  void StartTransaction() {
    txn_man_->StartTransaction(Transaction::Type::USER, 0, IsolationLevel::SERIALIZABLE, Transaction::Mode::OLTP);
  }

  /* Inserts the keys step, 2 * step, ..., count * step */
  void Preload(u8 count, size_t value_size, u8 step = 1) {
    StartTransaction();
    for (u8 idx = 1; idx <= count; idx++) {
      u8 key = idx * step;
      auto k = Key(key);
      ASSERT_EQ(tree_->Insert(k, Value(key, value_size)), OpResult::OK);
    }
    txn_man_->CommitTransaction();
  }

  /* Runs fn in a transaction of another worker, while the transaction of the test thread stays open */
  void RunInOtherWorker(const std::function<void()> &fn) {
    std::thread worker([&]() {
      LeanStore::worker_thread_id = OTHER_WORKER;
      fn();
    });
    worker.join();
  }

  auto Root() -> pageid_t {
    sync::OptimisticGuard<storage::MetadataPage> meta(buffer_.get(), METADATA_PAGE_ID);
    return meta->GetRoot(0);
  }

  auto LockState(pageid_t pid) -> PageLockState & {
    return *buffer_->BufferFramePage(pid).lock_state;
  }

  /* All leaves ever created (live, retired, evicted): exactly the pages with a lock state */
  auto Leaves() -> std::vector<pageid_t> {
    std::vector<pageid_t> leaves;
    for (pageid_t pid = 1; pid < N_PAGES; pid++) {
      if (buffer_->BufferFramePage(pid).lock_state != nullptr) {
        leaves.push_back(pid);
      }
    }
    return leaves;
  }

  /* The loaded leaves whose table holds at least one lock */
  auto LeavesWithLocks() -> std::vector<pageid_t> {
    std::vector<pageid_t> leaves;
    for (auto pid : Leaves()) {
      auto guard = LockState(pid).Lock();
      auto table = guard.IsInMemory() ? guard.GetLockTableIfExists() : nullptr;
      if (table != nullptr && !table->IsEmpty()) {
        leaves.push_back(pid);
      }
    }
    return leaves;
  }

  void Evict(const std::vector<pageid_t> &pids) {
    auto all_evicted = [&]() {
      for (auto pid : pids) {
        if (buffer_->GetPageState(pid).LockState() != sync::PageState::EVICTED) {
          return false;
        }
      }
      return true;
    };

    for (auto attempt = 0; attempt < 1000 && !all_evicted(); attempt++) {
      buffer_->Evict();
    }
    ASSERT_TRUE(all_evicted());
  }

  void ExpectState(pageid_t pid, PageLockState::State state) {
    auto guard = LockState(pid).Lock();
    EXPECT_EQ(guard.GetState(), state);
  }

  /* Every loaded leaf has an empty table */
  void ExpectNoLocksInMemory() {
    EXPECT_THAT(LeavesWithLocks(), ::testing::IsEmpty());
  }

  void ExpectValue(u8 key, const std::vector<u8> &value) {
    auto k = Key(key);
    auto ret = tree_->LookUp(k, [&](std::span<const u8> payload) {
      EXPECT_THAT(payload, ElementsAreArray(value));
    });
    ASSERT_EQ(ret, OpResult::OK);
  }

  /**
   * In transactions of the other worker: writing `key` conflicts with the lock of the test thread,
   * reading it conflicts only with an exclusive one
   */
  void ExpectConflictInOtherWorker(u8 key, bool held_exclusive) {
    RunInOtherWorker([&]() {
      auto k = Key(key);

      StartTransaction();
      auto expected = held_exclusive ? OpResult::ABORT_TX : OpResult::OK;
      EXPECT_EQ(tree_->LookUp(k, [](std::span<const u8>) {}), expected);
      txn_man_->AbortTransaction();

      StartTransaction();
      Update(key, Value(key, 1), OpResult::ABORT_TX);
      txn_man_->AbortTransaction();
    });
  }

  void Update(u8 key, const std::vector<u8> &value, OpResult expected) {
    auto k = Key(key);
    ASSERT_EQ(tree_->Update(k, value, {}), expected);
  }

  void Insert(u8 key, const std::vector<u8> &value) {
    auto k = Key(key);
    ASSERT_EQ(tree_->Insert(k, value), OpResult::OK);
  }

  void Remove(u8 key) {
    auto k = Key(key);
    ASSERT_EQ(tree_->Remove(k), OpResult::OK);
  }
};

/* The locks of an evicted leaf are released in the store, without loading the leaf */
TEST_F(TestSVCCInPageEviction, ReleaseWhileEvicted) {
  Preload(4, 1);
  auto leaf = Root();

  StartTransaction();
  ExpectValue(1, Value(1, 1));
  Update(2, Value(2, 2), OpResult::OK);
  EXPECT_THAT(LeavesWithLocks(), ElementsAreArray({leaf}));

  Evict({leaf});
  ExpectState(leaf, PageLockState::State::EVICTED_WITH_LOCKS);

  txn_man_->CommitTransaction();
  EXPECT_EQ(buffer_->GetPageState(leaf).LockState(), sync::PageState::EVICTED);

  // Loading the leaf brings back an empty table: the store has nothing left for it
  StartTransaction();
  ExpectValue(2, Value(2, 2));
  txn_man_->CommitTransaction();
  ExpectState(leaf, PageLockState::State::IN_MEMORY);
  ExpectNoLocksInMemory();
}

/* The locks still held come back into the table when the leaf is loaded, and are released there */
TEST_F(TestSVCCInPageEviction, LocksComeBackOnLoad) {
  Preload(4, 1);
  auto leaf = Root();

  StartTransaction();
  ExpectValue(1, Value(1, 1));
  Update(2, Value(2, 2), OpResult::OK);

  Evict({leaf});
  ExpectState(leaf, PageLockState::State::EVICTED_WITH_LOCKS);

  // Reading another key of the leaf loads it
  ExpectValue(3, Value(3, 1));
  ExpectState(leaf, PageLockState::State::IN_MEMORY);
  EXPECT_THAT(LeavesWithLocks(), ElementsAreArray({leaf}));

  txn_man_->CommitTransaction();
  ExpectNoLocksInMemory();
}

/* Abort after eviction: the undo loads the leaf, its locks come back, then they are released */
TEST_F(TestSVCCInPageEviction, AbortAfterEviction) {
  Preload(4, 1);
  auto leaf = Root();

  StartTransaction();
  Update(2, Value(2, 2), OpResult::OK);

  Evict({leaf});
  ExpectState(leaf, PageLockState::State::EVICTED_WITH_LOCKS);

  txn_man_->AbortTransaction();
  ExpectState(leaf, PageLockState::State::IN_MEMORY);
  ExpectNoLocksInMemory();

  StartTransaction();
  ExpectValue(2, Value(2, 1));
  txn_man_->CommitTransaction();
}

/* Another transaction loads the evicted leaf: the restored locks conflict with it */
TEST_F(TestSVCCInPageEviction, ConflictWithRestoredLocks) {
  Preload(4, 1);
  auto leaf = Root();

  StartTransaction();
  ExpectValue(1, Value(1, 1));
  Update(2, Value(2, 2), OpResult::OK);

  Evict({leaf});
  RunInOtherWorker([&]() {
    // A shared lock is compatible with a shared lock, an exclusive one is not
    StartTransaction();
    ExpectValue(1, Value(1, 1));
    Update(1, Value(1, 2), OpResult::ABORT_TX);
    txn_man_->AbortTransaction();
  });
  ExpectConflictInOtherWorker(2, true);

  // A key nobody holds is free
  RunInOtherWorker([&]() {
    StartTransaction();
    Update(3, Value(3, 3), OpResult::OK);
    txn_man_->CommitTransaction();
  });

  txn_man_->CommitTransaction();

  RunInOtherWorker([&]() {
    StartTransaction();
    Update(2, Value(2, 3), OpResult::OK);
    txn_man_->CommitTransaction();
  });
  ExpectNoLocksInMemory();
}

/**
 * A split moves the locks of the upper half to the new leaf. That leaf is evicted: the locks go to the store,
 * the release of the moved locks misses the hint leaf and descends to the new one
 */
TEST_F(TestSVCCInPageEviction, SplitThenEvict) {
  // Even keys only: the other worker inserts odd ones in between
  Preload(60, LARGE_VALUE, 2);
  auto leaves_before = Leaves();

  // Lock every key of the last leaf
  StartTransaction();
  u8 last = 120;
  ExpectValue(last, Value(last, LARGE_VALUE));
  ASSERT_EQ(LeavesWithLocks().size(), 1U);
  auto hint = LeavesWithLocks()[0];
  auto hint_count = reinterpret_cast<storage::BTreeNode *>(buffer_->ToPtr(hint))->header.count;
  u8 first = last - 2 * (hint_count - 1);
  std::vector<u8> locked;
  for (u8 key = first; key <= last; key += 2) {
    ExpectValue(key, Value(key, LARGE_VALUE));
    locked.push_back(key);
  }

  // Inserts of the other worker into the range of the last leaf split it in the middle of the locked keys
  RunInOtherWorker([&]() {
    StartTransaction();
    for (u8 key = first + 1; key < last; key += 2) {
      Insert(key, Value(key, LARGE_VALUE));
    }
    txn_man_->CommitTransaction();
  });
  ASSERT_GT(Leaves().size(), leaves_before.size());
  ASSERT_GE(LeavesWithLocks().size(), 2U);

  auto with_locks = LeavesWithLocks();
  Evict(with_locks);
  for (auto pid : with_locks) {
    ExpectState(pid, PageLockState::State::EVICTED_WITH_LOCKS);
  }

  for (auto key : locked) {
    ExpectConflictInOtherWorker(key, false);
  }

  Evict(with_locks);
  txn_man_->CommitTransaction();
  for (auto pid : with_locks) {
    EXPECT_EQ(buffer_->GetPageState(pid).LockState(), sync::PageState::EVICTED);
  }

  // Loading the leaves again: nothing left in the store
  StartTransaction();
  for (auto key : locked) {
    ExpectValue(key, Value(key, LARGE_VALUE));
  }
  txn_man_->CommitTransaction();
  ExpectNoLocksInMemory();
}

/**
 * A merge moves the locks of the left leaf to its right sibling and retires the left one.
 * The release misses the retired hint leaf and descends to the merged one, which is evicted
 */
TEST_F(TestSVCCInPageEviction, MergeThenEvict) {
  Preload(60, LARGE_VALUE);

  // Lock the first key, it is in the leftmost leaf
  StartTransaction();
  ExpectValue(1, Value(1, LARGE_VALUE));
  ASSERT_EQ(LeavesWithLocks().size(), 1U);
  auto hint = LeavesWithLocks()[0];
  auto hint_count = reinterpret_cast<storage::BTreeNode *>(buffer_->ToPtr(hint))->header.count;
  ASSERT_GE(hint_count, 2);

  // The other worker empties the leftmost leaf but the first key: it merges into its right sibling
  RunInOtherWorker([&]() {
    StartTransaction();
    for (u8 key = 2; key <= hint_count; key++) {
      Remove(key);
    }
    txn_man_->CommitTransaction();
  });
  ExpectState(hint, PageLockState::State::RETIRED);

  ASSERT_EQ(LeavesWithLocks().size(), 1U);
  auto merged = LeavesWithLocks()[0];
  EXPECT_NE(merged, hint);

  // The retired leaf is evicted as well: it has no locks to spill and stays retired
  Evict({hint, merged});
  ExpectState(hint, PageLockState::State::RETIRED);
  ExpectState(merged, PageLockState::State::EVICTED_WITH_LOCKS);

  ExpectConflictInOtherWorker(1, false);

  Evict({merged});
  txn_man_->CommitTransaction();
  EXPECT_EQ(buffer_->GetPageState(merged).LockState(), sync::PageState::EVICTED);

  StartTransaction();
  ExpectValue(1, Value(1, LARGE_VALUE));
  txn_man_->CommitTransaction();
  ExpectNoLocksInMemory();
}

/**
 * ExtractAscending locks all the matching records of a leaf before it consumes any:
 * a conflict on one of them leaves the whole leaf as it is.
 * Disabled: ExtractAscending still consumes the records before the conflict
 */
TEST_F(TestSVCCInPageEviction, DISABLED_ExtractWithConflictRemovesNothing) {
  Preload(5, 1);

  StartTransaction();
  Update(3, Value(3, 2), OpResult::OK);

  RunInOtherWorker([&]() {
    auto from = Key(1);
    u32 consumed_count = 0;

    StartTransaction();
    auto ret = tree_->ExtractAscending(
      from,
      [](std::span<const u8>) {
        return true;
      },
      [&](std::span<u8>, std::span<u8>) {
        consumed_count++;
        return true;
      });
    EXPECT_EQ(ret, OpResult::ABORT_TX);
    EXPECT_EQ(consumed_count, 0U);
    txn_man_->AbortTransaction();
  });

  for (u8 key = 1; key <= 5; key++) {
    ExpectValue(key, (key == 3) ? Value(3, 2) : Value(key, 1));
  }
  txn_man_->CommitTransaction();
  ExpectNoLocksInMemory();
}

}  // namespace leanstore

auto main(int argc, char **argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);
  FLAGS_worker_count = 4;
  FLAGS_wal_enable = false;
  FLAGS_txn_mvcc = false;
  FLAGS_txn_lock_inpage = true;
  FLAGS_wal_enable_recovery = false;

  google::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
