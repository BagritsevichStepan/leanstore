#pragma once

#include "leanstore/config.h"

namespace leanstore::transaction {

/**
 * @brief The three concurrency control schemes, distinguished by where the CC state lives.
 * They are mutually exclusive, which TransactionManager's constructor enforces.
 */

/* Multi-version: commit timestamp in the KV header, older versions in an in-memory version chain */
inline auto IsMvcc() -> bool { return FLAGS_txn_mvcc; }

/* Single-version 2PL: lock state in an external hash table, nothing of it lives in the page */
inline auto IsSvccLockTable() -> bool { return !FLAGS_txn_mvcc && !FLAGS_txn_lock_inpage; }

/* Single-version 2PL (No-Wait): lock state in a lock table per B-tree leaf, see svcc::PageLockTable */
inline auto IsSvccInPageLock() -> bool { return !FLAGS_txn_mvcc && FLAGS_txn_lock_inpage; }

}  // namespace leanstore::transaction
