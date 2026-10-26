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
 */
class PageLockState {
 public:
  enum class State : u8 {
    IN_MEMORY,           // the page is in memory, its locks (if any) are in its table
    EVICTED,             // the page is evicted, it holds no locks
    EVICTED_WITH_LOCKS,  // the page is evicted, its locks are in the EvictedLockStore
    RETIRED              // the page was merged away
  };

  // Exclusive access to the state and the lock table, for as long as the guard lives.
  class Guard {
   public:
    explicit Guard(PageLockState &owner);

    auto GetState() const -> State;
    auto IsInMemory() const -> bool;
    auto IsEvictedWithLocks() const -> bool;

    /* Returns the lock table, which must exist */
    auto GetLockTable() -> PageLockTable &;
    /* Returns the lock table, created if the page has none yet */
    auto GetOrCreateLockTable() -> PageLockTable &;
    /* Returns the lock table, nullptr if the page has none */
    auto GetLockTableIfExists() -> PageLockTable *;
    

    void Retire();
    /* EVICTED_WITH_LOCKS -> EVICTED */
    void DropEvictedLocks();

   private:
    PageLockState &owner_;
    sync::HybridGuard guard_;
  };

  PageLockState() = default;

  PageLockState(const PageLockState &) = delete;
  auto operator=(const PageLockState &) -> PageLockState & = delete;

  auto Lock() -> Guard;

  /* IN_MEMORY -> EVICTED(_WITH_LOCKS): spill() moves the locks (if any) out of the table, the table is deleted */
  template <typename SpillFn>
  void Evict(SpillFn &&spill);

  /* EVICTED(_WITH_LOCKS) -> IN_MEMORY: restore() fills a new table with the locks moved out by Evict(), if any */
  template <typename RestoreFn>
  void Restore(RestoreFn &&restore);

 private:
  sync::HybridLatch latch_;
  State state_ = State::IN_MEMORY;
  std::unique_ptr<PageLockTable> table_;
};

// Implementation
inline PageLockState::Guard::Guard(PageLockState &owner)
    : owner_(owner), guard_(&owner.latch_, sync::GuardMode::EXCLUSIVE) {}

inline auto PageLockState::Guard::GetState() const -> State {
  return owner_.state_;
}

inline auto PageLockState::Guard::IsInMemory() const -> bool {
  return owner_.state_ == State::IN_MEMORY;
}

inline auto PageLockState::Guard::IsEvictedWithLocks() const -> bool {
  return owner_.state_ == State::EVICTED_WITH_LOCKS;
}

inline auto PageLockState::Guard::GetOrCreateLockTable() -> PageLockTable & {
  Ensure(IsInMemory());

  if (owner_.table_ == nullptr) {
    owner_.table_ = std::make_unique<PageLockTable>();
  }

  return *owner_.table_;
}

inline auto PageLockState::Guard::GetLockTableIfExists() -> PageLockTable * {
  Ensure(IsInMemory());
  return owner_.table_.get();
}

inline auto PageLockState::Guard::GetLockTable() -> PageLockTable & {
  auto table = GetLockTableIfExists();
  Ensure(table != nullptr);
  return *table;
}

inline void PageLockState::Guard::Retire() {
  Ensure(IsInMemory());
  owner_.table_.reset();
  owner_.state_ = State::RETIRED;
}

inline void PageLockState::Guard::DropEvictedLocks() {
  Ensure(IsEvictedWithLocks());
  owner_.state_ = State::EVICTED;
}

inline auto PageLockState::Lock() -> Guard {
  return Guard(*this);
}

template <typename SpillFn>
void PageLockState::Evict(SpillFn &&spill) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  
  if (state_ == State::RETIRED) {
    return;
  }

  Ensure(state_ == State::IN_MEMORY);

  if (table_ != nullptr && !table_->IsEmpty()) {
    spill(*table_);
    state_ = State::EVICTED_WITH_LOCKS;
  } else {
    state_ = State::EVICTED;
  }

  table_.reset();
}

template <typename RestoreFn>
void PageLockState::Restore(RestoreFn &&restore) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  
  if (state_ == State::RETIRED) {
    return;
  }

  Ensure(state_ == State::EVICTED || state_ == State::EVICTED_WITH_LOCKS);

  if (state_ == State::EVICTED_WITH_LOCKS) {
    table_ = std::make_unique<PageLockTable>();
    restore(*table_);
  }

  state_ = State::IN_MEMORY;
}

}  // namespace leanstore::transaction::svcc
