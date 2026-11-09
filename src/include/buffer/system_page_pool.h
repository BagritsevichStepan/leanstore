#pragma once

#include "buffer/resident_set.h"
#include "common/typedefs.h"

#include <atomic>

namespace leanstore::buffer {

class BufferManager;

/**
 * The pool of the system pages: the pages of internal structures, e.g. the B-tree of the locks of evicted pages.
 *
 * The system pages are the pids [first_pid, first_pid + virtual_count) of the buffer manager. The pool has its own
 * memory budget and its own clock: the eviction of the buffer manager never picks a system page, the eviction of the
 * pool only picks system pages. A system page holds no locks, so its eviction spills nothing: an operation on the
 * structure may evict pages of the pool without any recursion into the structure.
 */
class SystemPagePool {
 public:
  /* Page ids of the system pages (--bm_system_virtual_gb), 0 without SVCC in-page locking: the pool is only for it */
  static auto VirtualCount() -> u64;
  /* Memory budget in pages (--bm_system_physical_mb), 0 without SVCC in-page locking */
  static auto PhysicalCount() -> u64;

  /* The memory budget is PhysicalCount() */
  SystemPagePool(BufferManager &buffer, pageid_t first_pid, u64 virtual_count);

  auto Contains(pageid_t page_id) const -> bool;

  /* A new system page, exclusively latched */
  auto AllocPage() -> pageid_t;

  /* Loads an evicted system page, which the caller latched exclusively */
  void HandlePageFault(pageid_t page_id);
  /* Evicts every system page in memory, e.g. to drop the cache before an experiment. Waits for latched ones */
  void EvictAll();

 private:
  static constexpr u64 MAX_LATCHED_PAGES_PER_THREAD = 4;
  static constexpr u64 SPARE_PAGES = 2;
  static constexpr u64 CLOCK_BATCH = 16;

  /* Takes the memory of one page, evicts a system page while the pool is over its budget */
  void ReservePage();
  /* Evicts one unlatched system page. false if there is none right now */
  auto EvictOne() -> bool;
  /* Evicts the page if it is still in the state state_and_version, writes it first if it is dirty */
  auto TryEvict(pageid_t page_id, u64 state_and_version) -> bool;

  BufferManager &buffer_;
  const pageid_t first_pid_;
  const u64 virtual_count_;
  const u64 physical_count_;

  ResidentPageSet resident_set_;  // the clock of the system pages in memory
  std::atomic<pageid_t> next_pid_;
  std::atomic<u64> used_count_{0};  // system pages in memory, or being loaded / allocated
};

// Implementation
inline auto SystemPagePool::Contains(pageid_t page_id) const -> bool {
  return page_id >= first_pid_ && page_id < first_pid_ + virtual_count_;
}

}  // namespace leanstore::buffer
