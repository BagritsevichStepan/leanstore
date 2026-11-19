#pragma once

#include "common/constants.h"
#include "common/exceptions.h"
#include "common/typedefs.h"
#include "common/utils.h"

#include <array>
#include <atomic>

namespace leanstore::transaction::svcc::inpage::detail {

/**
 * @brief One wait slot per worker: a worker queued for a lock sleeps on its slot until a holder grants it.
 */
class WaiterWorkersSlots {
 public:
  /* Spins until WakeWorker() is called for the specified worker. */
  void Wait(wid_t worker_id);
  /* Grants the lock to the specified waiting worker. */
  void WakeWorker(wid_t worker_id);

 private:
  struct alignas(CPU_CACHELINE_SIZE) WorkerSlot {
    std::atomic<bool> granted{false};
  };

  std::array<WorkerSlot, MAX_NUMBER_OF_WORKER> slots_;
};

// Implementation
inline void WaiterWorkersSlots::Wait(wid_t worker_id) {
  auto &granted = slots_[worker_id].granted;

  for (u64 spin = 0; !granted.load(std::memory_order_acquire); spin++) {
    AsmYield(spin);
  }

  granted.store(false, std::memory_order_relaxed);
}

inline void WaiterWorkersSlots::WakeWorker(wid_t worker_id) {
  slots_[worker_id].granted.store(true, std::memory_order_release);
}

}  // namespace leanstore::transaction::svcc::inpage::detail
