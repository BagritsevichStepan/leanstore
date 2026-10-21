#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "sync/hybrid_guard.h"
#include "sync/hybrid_latch.h"
#include "transaction/svcc/page_lock_table.h"

#include <memory>

namespace leanstore::transaction::svcc {

/**
 * @brief Lock state of a single page that holds locks (SVCC in-page locking).
 *
 * It outlives the residency of its page: the page may be evicted and loaded again many times.
 * Every access to the state and to the lock table goes through the latch of this object, see Guard.
 *
 * State transitions (each one under the latch, and under an X-latch on the page):
 *  - IN_MEMORY -> EVICTED: the page is evicted, its locks are moved out of the table
 *  - EVICTED -> IN_MEMORY: the page is loaded, its locks are moved back into a new table
 *  - IN_MEMORY -> RETIRED: the page no longer holds locks and never will again,
 *                          its locks were handed over to another page
 *
 * A RETIRED object is never deleted: a thread may still wait on its latch.
 */
class PageLockState {
 public:
  enum class State : u8 {
    IN_MEMORY,
    EVICTED,
    RETIRED
  };

  // Exclusive access to the state and the lock table, for as long as the guard lives.
  class Guard {
   public:
    explicit Guard(PageLockState &owner);

    auto GetState() const -> State;
    auto GetLockTable() -> PageLockTable &;

    void Retire();

   private:
    PageLockState &owner_;
    sync::HybridGuard guard_;
  };

  PageLockState() = default;

  PageLockState(const PageLockState &) = delete;
  auto operator=(const PageLockState &) -> PageLockState & = delete;

  auto Lock() -> Guard;

  /* IN_MEMORY -> EVICTED: spill() moves the locks out of the table, then the table is deleted */
  template <typename SpillFn>
  void Evict(SpillFn &&spill);

  /* EVICTED -> IN_MEMORY: restore() fills a new table with the locks moved out by Evict() */
  template <typename RestoreFn>
  void Restore(RestoreFn &&restore);

 private:
  sync::HybridLatch latch_;
  State state_ = State::IN_MEMORY;
  std::unique_ptr<PageLockTable> table_ = std::make_unique<PageLockTable>();
};

// Implementation
inline PageLockState::Guard::Guard(PageLockState &owner)
    : owner_(owner), guard_(&owner.latch_, sync::GuardMode::EXCLUSIVE) {}

inline auto PageLockState::Guard::GetState() const -> State {
  return owner_.state_;
}

inline auto PageLockState::Guard::GetLockTable() -> PageLockTable & {
  Ensure(owner_.state_ == State::IN_MEMORY);
  return *owner_.table_;
}

inline void PageLockState::Guard::Retire() {
  Ensure(owner_.state_ == State::IN_MEMORY);
  owner_.table_.reset();
  owner_.state_ = State::RETIRED;
}

inline auto PageLockState::Lock() -> Guard {
  return Guard(*this);
}

template <typename SpillFn>
void PageLockState::Evict(SpillFn &&spill) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  Ensure(state_ == State::IN_MEMORY);

  spill(*table_);

  table_.reset();
  state_ = State::EVICTED;
}

template <typename RestoreFn>
void PageLockState::Restore(RestoreFn &&restore) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  Ensure(state_ == State::EVICTED);

  table_ = std::make_unique<PageLockTable>();
  restore(*table_);
  state_ = State::IN_MEMORY;
}

}  // namespace leanstore::transaction::svcc
