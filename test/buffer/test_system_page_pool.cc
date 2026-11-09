#include "buffer/buffer_manager.h"
#include "common/exceptions.h"
#include "common/typedefs.h"
#include "storage/page.h"
#include "sync/page_guard/exclusive_guard.h"
#include "sync/page_guard/shared_guard.h"
#include "sync/page_state.h"
#include "test/base_test.h"

#include "gtest/gtest.h"

#include <cstring>
#include <vector>

namespace leanstore::buffer {

/**
 * SVCC in-page locking: the system pages, e.g. of the tree of the locks of evicted pages.
 * They follow the database pages, with a memory budget of their own (1 MB = 256 pages here)
 */
class TestSystemPagePool : public BaseTest {
 protected:
  static constexpr u64 BUDGET_PAGES = 1 * MB / PAGE_SIZE;

  void SetUp() override {
    BaseTest::SetupTestFile();
  }

  /* The body of a page, i.e. without its header */
  static auto Body(storage::Page *page) -> u8 * {
    return reinterpret_cast<u8 *>(page) + sizeof(storage::PageHeader);
  }

  static void Stamp(storage::Page *page, pageid_t pid) {
    std::memset(Body(page), static_cast<u8>(pid), PAGE_SIZE - sizeof(storage::PageHeader));
    std::memcpy(Body(page), &pid, sizeof(pid));
  }

  static void ExpectStamp(storage::Page *page, pageid_t pid) {
    pageid_t stamped_pid;
    std::memcpy(&stamped_pid, Body(page), sizeof(stamped_pid));
    EXPECT_EQ(stamped_pid, pid);

    auto last_byte = Body(page)[PAGE_SIZE - sizeof(storage::PageHeader) - 1];
    EXPECT_EQ(last_byte, static_cast<u8>(pid));
  }

  /* Allocates a system page, stamps it with its pid and unlatches it */
  auto AllocStampedSystemPage() -> pageid_t {
    sync::ExclusiveGuard<storage::Page> guard(buffer_.get(), buffer_->AllocPage(true));
    Stamp(guard.Ptr(), guard.PageID());
    return guard.PageID();
  }
};

TEST_F(TestSystemPagePool, SystemPagesFollowDatabasePages) {
  for (auto idx = 0; idx < 10; idx++) {
    auto pid = AllocStampedSystemPage();
    EXPECT_GE(pid, N_PAGES);
    EXPECT_TRUE(buffer_->IsSystemPage(pid));
    EXPECT_EQ(buffer_->GetPageState(pid).LockState(), sync::PageState::UNLOCKED);
  }

  auto database_page = buffer_->AllocPage();
  auto database_pid = buffer_->ToPID(database_page);
  EXPECT_LT(database_pid, N_PAGES);
  EXPECT_FALSE(buffer_->IsSystemPage(database_pid));
  buffer_->UnfixExclusive(database_pid);
}

TEST_F(TestSystemPagePool, EvictsAndLoadsBeyondBudget) {
  // Three times the budget: the pool evicts its own pages to disk and loads them back
  std::vector<pageid_t> pids;
  for (u64 idx = 0; idx < 3 * BUDGET_PAGES; idx++) {
    pids.push_back(AllocStampedSystemPage());
  }

  u64 evicted_count = 0;
  for (auto pid : pids) {
    if (buffer_->GetPageState(pid).LockState() == sync::PageState::EVICTED) {
      evicted_count++;
    }
  }
  EXPECT_GE(evicted_count, 2 * BUDGET_PAGES - 2);

  // Twice: the second pass loads again the pages evicted by the first one
  for (auto pass = 0; pass < 2; pass++) {
    for (auto pid : pids) {
      sync::SharedGuard<storage::Page> guard(buffer_.get(), pid);
      ExpectStamp(guard.Ptr(), pid);
    }
  }
}

TEST_F(TestSystemPagePool, ModifiedPageIsWrittenBack) {
  std::vector<pageid_t> pids;
  for (u64 idx = 0; idx < 2 * BUDGET_PAGES; idx++) {
    pids.push_back(AllocStampedSystemPage());
  }

  // The first page was evicted, load it, change it, then evict it again by allocating more pages
  auto first_pid = pids.front();
  {
    sync::ExclusiveGuard<storage::Page> guard(buffer_.get(), first_pid);
    ExpectStamp(guard.Ptr(), first_pid);
    Stamp(guard.Ptr(), first_pid + 1);
  }

  for (u64 idx = 0; idx < 2 * BUDGET_PAGES; idx++) {
    AllocStampedSystemPage();
  }
  EXPECT_EQ(buffer_->GetPageState(first_pid).LockState(), sync::PageState::EVICTED);

  sync::SharedGuard<storage::Page> guard(buffer_.get(), first_pid);
  ExpectStamp(guard.Ptr(), first_pid + 1);
}

TEST_F(TestSystemPagePool, BudgetTooSmallForThreads) {
  // 4 * (100 + 1) + 2 pages are needed, the budget is 256
  auto worker_count = FLAGS_worker_count;
  FLAGS_worker_count = 100;

  std::atomic<bool> is_running = true;
  EXPECT_THROW(BufferManager(N_PAGES, PHYSICAL_CAP, EXTRA_NO_PG, EVICT_SIZE, is_running), ex::GenericException);

  FLAGS_worker_count = worker_count;
}

}  // namespace leanstore::buffer

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
