#pragma once

#include "common/typedefs.h"
#include "transaction/svcc/page_lock_state.h"

#include <atomic>
#include <bitset>
#include <memory>

namespace leanstore::buffer {

/**
 * @brief Run-time context of all DB pages
 */
struct BufferFrame {
  wid_t last_writer;     // the id of last worker who modifies this page since its last flush
  u64 last_written_gsn;  // the last gsn that this extent/page was written to storage

  // SVCC in-page locking: nullptr if the page holds no locks
  std::unique_ptr<transaction::svcc::PageLockState> lock_state;

  void Init();
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
  lock_state.reset();
}

}  // namespace leanstore::buffer