#pragma once

#include "common/typedefs.h"
#include "leanstore/config.h"

namespace leanstore::transaction::svcc::inpage {

/**
 * @brief The result of a lock acquisition attempt.
 */
enum class LockResult : u8 {
  ACQUIRED,
  ABORT,
  MUST_WAIT,
};

/**
 * @brief Returns true if the lock policy is No-Wait
 */
inline auto IsNoWait() -> bool {
  return FLAGS_txn_inpage_lock_policy == "nowait";
}

/**
 * @brief Lock parameters that are used to acquire a lock
 */
struct LockParams {
  wid_t worker_id;
  timestamp_t txn_ts;
  bool exclusive;
};

}  // namespace leanstore::transaction::svcc::inpage
