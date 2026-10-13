#pragma once

#include "common/typedefs.h"
#include "transaction/svcc/page_lock_table.h"

#include <atomic>
#include <bitset>

namespace leanstore::buffer {

/**
 * @brief Run-time context of all DB pages
 */
struct BufferFrame {
  wid_t last_writer;     // the id of last worker who modifies this page since its last flush
  u64 last_written_gsn;  // the last gsn that this extent/page was written to storage

  void Init();

  auto GetLockTable() -> transaction::svcc::PageLockTable *;
  auto GetOrCreateLockTable() -> transaction::svcc::PageLockTable *;

 private:
  /* Created lazily and never replaced while the page lives.
   * It lives in the frame, not in the page, hence survives eviction */
  std::atomic<transaction::svcc::PageLockTable *> lock_table_;
};

/**
 * @brief Similar to Buffer Frame, but for Extent pages
 */
struct alignas(8) ExtentFrame {
  std::atomic<bool> prevent_evict;
  u64 extent_size : 48;

  void Init() {
    prevent_evict = false;
    extent_size   = 1;
  }
};

static_assert(sizeof(ExtentFrame) == 8);

// Implementation
inline void BufferFrame::Init() {
  last_writer = 0;
  last_written_gsn = 0;
  lock_table_.store(nullptr, std::memory_order_release);
}

inline auto BufferFrame::GetLockTable() -> transaction::svcc::PageLockTable * {
  return lock_table_.load(std::memory_order_acquire);
}

inline auto BufferFrame::GetOrCreateLockTable() -> transaction::svcc::PageLockTable * {
  auto table = lock_table_.load(std::memory_order_acquire);
  if (table != nullptr) {
    return table;
  }

  auto new_table = new transaction::svcc::PageLockTable();
  if (lock_table_.compare_exchange_strong(table, new_table, std::memory_order_acq_rel)) {
    return new_table;
  }

  // Another thread created the table first
  delete new_table;
  return table;
}

}  // namespace leanstore::buffer