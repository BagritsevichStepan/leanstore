#include "buffer/system_page_pool.h"
#include "buffer/buffer_manager.h"
#include "common/exceptions.h"
#include "common/utils.h"
#include "leanstore/config.h"
#include "transaction/cc_mode.h"

#include "fmt/format.h"
#include "spdlog/spdlog.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cassert>

namespace leanstore::buffer {

auto SystemPagePool::VirtualCount() -> u64 {
  return transaction::IsSvccInPageLock() ? FLAGS_bm_system_virtual_gb * GB / PAGE_SIZE : 0;
}

auto SystemPagePool::PhysicalCount() -> u64 {
  return transaction::IsSvccInPageLock() ? FLAGS_bm_system_physical_mb * MB / PAGE_SIZE : 0;
}

SystemPagePool::SystemPagePool(BufferManager &buffer, pageid_t first_pid, u64 virtual_count)
    : buffer_(buffer),
      first_pid_(first_pid),
      virtual_count_(virtual_count),
      physical_count_(PhysicalCount()),
      resident_set_(physical_count_),
      next_pid_(first_pid) {
  const u64 threads = FLAGS_worker_count + FLAGS_page_provider_thread + 1;
  const u64 min_physical_count = MAX_LATCHED_PAGES_PER_THREAD * threads + SPARE_PAGES;
  if (physical_count_ < min_physical_count) {
    throw leanstore::ex::GenericException(
      fmt::format("System page pool of {} pages, {} threads need at least {}", physical_count_, threads,
                  min_physical_count));
  }
  Ensure(physical_count_ <= virtual_count_);

  spdlog::info("System pages: VirtualCount({}) from pid {}, PhysCount({})", virtual_count_, first_pid_,
               physical_count_);
}

auto SystemPagePool::AllocPage() -> pageid_t {
  auto pid = next_pid_.fetch_add(1);
  Ensure(Contains(pid));
  ReservePage();

  // A page never used before: unlatched
  auto &ps = buffer_.GetPageState(pid);
  u64 v = ps.StateAndVersion().load();
  Ensure(ps.TryLockExclusive(v));

  buffer_.frame_[pid].Init();
  // last_written_gsn (0) < p_gsn: dirty until it is written
  buffer_.mem_[pid].p_gsn = 1;
  resident_set_.Insert(pid);
  return pid;
}

void SystemPagePool::HandlePageFault(pageid_t page_id) {
  assert(Contains(page_id));
  assert(buffer_.GetPageState(page_id).LockState() == sync::PageStateMode::EXCLUSIVE);
  ReservePage();

  auto ret = pread(buffer_.blockfd_, &buffer_.mem_[page_id], PAGE_SIZE, page_id * PAGE_SIZE);
  Ensure(ret == static_cast<ssize_t>(PAGE_SIZE));
  resident_set_.Insert(page_id);
}

void SystemPagePool::EvictAll() {
  while (used_count_.load() > 0) {
    if (!EvictOne()) {
      AsmYield();
    }
  }
}

void SystemPagePool::ReservePage() {
  used_count_++;
  // The pool may go over its budget by the threads reserving at the same time, the resident set has room for them
  while (used_count_.load() > physical_count_) {
    if (!EvictOne()) {
      AsmYield();
    }
  }
}

auto SystemPagePool::EvictOne() -> bool {
  // More than 2 rounds of the clock: the slots of the resident set are at most 3 * its capacity
  const u64 batches = 6 * resident_set_.Capacity() / CLOCK_BATCH + 1;
  bool evicted = false;

  // Clock: the 1st round marks the unlatched pages, the 2nd one evicts a page that stayed marked, i.e. was not used
  for (u64 idx = 0; idx < batches && !evicted; idx++) {
    resident_set_.IterateClockBatch(CLOCK_BATCH, [&](pageid_t pid) {
      if (evicted) {
        return;
      }

      auto &ps = buffer_.GetPageState(pid);
      u64 v = ps.StateAndVersion();
      switch (sync::PageState::LockState(v)) {
        case sync::PageState::MARKED:
          evicted = TryEvict(pid, v);
          break;
        case sync::PageState::UNLOCKED:
          ps.TryMark(v);
          break;
        default:
          break;
      }
    });
  }

  if (evicted) {
    return true;
  }

  // No page stayed marked: every access unmarks a page, and in a small pool the hot pages (e.g. the inner nodes) are
  // accessed all the time. Evict any unlatched page then: there is one, the budget is above the latched pages
  for (u64 idx = 0; idx < batches && !evicted; idx++) {
    resident_set_.IterateClockBatch(CLOCK_BATCH, [&](pageid_t pid) {
      if (evicted) {
        return;
      }

      auto &ps = buffer_.GetPageState(pid);
      u64 v = ps.StateAndVersion();
      auto state = sync::PageState::LockState(v);
      if (state == sync::PageState::UNLOCKED || state == sync::PageState::MARKED) {
        evicted = TryEvict(pid, v);
      }
    });
  }

  return evicted;
}

auto SystemPagePool::TryEvict(pageid_t page_id, u64 state_and_version) -> bool {
  auto &ps = buffer_.GetPageState(page_id);
  if (!ps.TryLockExclusive(state_and_version)) {
    return false;
  }

  // Exclusively latched: no writer, the optimistic readers restart on the new version
  auto page = &buffer_.mem_[page_id];
  if (buffer_.PageIsDirty(page_id)) {
    auto ret = pwrite(buffer_.blockfd_, page, PAGE_SIZE, page_id * PAGE_SIZE);
    Ensure(ret == static_cast<ssize_t>(PAGE_SIZE));
    buffer_.frame_[page_id].last_written_gsn = page->p_gsn;
  }

  madvise(page, PAGE_SIZE, MADV_DONTNEED);
  Ensure(resident_set_.Remove(page_id));
  used_count_--;
  ps.UnlockExclusiveAndEvict();
  return true;
}

}  // namespace leanstore::buffer
