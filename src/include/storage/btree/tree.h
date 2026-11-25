#pragma once

#include "buffer/buffer_manager.h"
#include "common/typedefs.h"
#include "leanstore/kv_interface.h"
#include "storage/btree/node.h"
#include "storage/btree/wal.h"
#include "storage/page.h"
#include "sync/page_guard/exclusive_guard.h"
#include "sync/page_guard/optimistic_guard.h"
#include "sync/page_guard/shared_guard.h"
#include "transaction/cc_mode.h"

#include <cstring>
#include <functional>
#include <span>
#include <utility>

namespace leanstore::storage {

using KeyPredicateFunc = std::function<bool(std::span<const u8>)>;
using ConsumeRecordFunc = std::function<void(std::span<const u8>, std::span<const u8>)>;

class BTree : public KVInterface {
 public:
  BTree(buffer::BufferManager *buffer_pool, u32 tree_slot, bool system = false);
  ~BTree() override = default;

  /* BTree config*/
  void SetComparisonOperator(ComparisonLambda cmp) override;

  /* All BTree operators */
  // -------------------------------------------------------------------------------------
  /* Public APIs for external use */
  auto LookUp(std::span<u8> key, const AccessPayloadFunc &read_cb) -> OpResult override;
  auto Insert(std::span<u8> key, std::span<const u8> payload) -> OpResult override;
  auto Remove(std::span<u8> key) -> OpResult override;
  auto Upsert(std::span<u8> key, std::span<const u8> payload) -> OpResult;
  auto Update(std::span<u8> key, std::span<const u8> payload, const AccessPayloadFunc &func) -> OpResult override;
  auto UpdateInPlace(std::span<u8> key, const ModifyPayloadFunc &func, FixedSizeDelta *delta) -> OpResult override;
  auto ScanAscending(std::span<u8> key, const AccessRecordFunc &fn) -> OpResult override;
  auto ScanDescending(std::span<u8> key, const AccessRecordFunc &fn) -> OpResult override;
  auto CountEntries() -> u64 override;
  auto SizeInMB() -> float override;
  auto LookUpBlob(std::span<const u8> blob_key, const ComparisonLambda &cmp, const AccessPayloadFunc &read_cb)
    -> bool override;

  // -------------------------------------------------------------------------------------
  /* APIs for use within LeanStore */
  auto IsNotEmpty() -> bool;
  auto CountPages() -> u64;
  auto GetTimestamp(std::span<u8> key) -> timestamp_t;
  void UpdateTimestamp(std::span<u8> key, timestamp_t commit_ts);

  /* SVCC in-page locking: release a lock the running transaction holds on key.
     Releases the lock of key, held by txn_ts, and returns the waiters to wake up */
  auto ReleaseInPageLock(timestamp_t txn_ts, std::span<const u8> key, pageid_t leaf_hint, bool exclusive)
    -> std::vector<wid_t>;

  /* API for system trees only. Consumes and removes the records matching `matches` in ascending order */
  auto ExtractAscending(std::span<u8> key, const KeyPredicateFunc &matches, const ConsumeRecordFunc &consume)
    -> OpResult;

 private:
  /* Iterate all pages utilities */
  auto IterateAllNodes(sync::OptimisticGuard<BTreeNode> &node, const std::function<u64(BTreeNode &)> &inner_fn,
                       const std::function<u64(BTreeNode &)> &leaf_fn) -> u64;
  auto IterateUntils(sync::OptimisticGuard<BTreeNode> &node, const std::function<bool(BTreeNode &)> &inner_fn,
                     const std::function<bool(BTreeNode &)> &leaf_fn) -> bool;

  void InitRoot(sync::ExclusiveGuard<MetadataPage> &meta_page);
  auto IsWalEnabled() const -> bool;

  auto IsMvcc() const -> bool;
  auto IsSvccLockTable() const -> bool;
  auto IsSvccInPageLock() const -> bool;

  /* Find Leaf Node storing the key */
  auto FindLeafOptimisticWithParent(std::span<u8> key)
    -> std::pair<sync::OptimisticGuard<BTreeNode>, sync::OptimisticGuard<BTreeNode>>;
  auto FindLeafOptimistic(std::span<u8> key) -> sync::OptimisticGuard<BTreeNode>;

  /* Split/Merge utilities */
  void TrySplit(sync::ExclusiveGuard<BTreeNode> &&parent, sync::ExclusiveGuard<BTreeNode> &&node);
  void EnsureSpaceForSplit(BTreeNode *to_split, std::span<u8> key);
  void TryMerge(sync::ExclusiveGuard<BTreeNode> &&parent, sync::ExclusiveGuard<BTreeNode> &&left,
                sync::ExclusiveGuard<BTreeNode> &&right, leng_t left_pos);
  void EnsureUnderfullInnersForMerge(BTreeNode *to_merge, std::span<u8> rep_key);

  /* Insert/Update utilities */
  auto InsertIntoLeaf(sync::OptimisticGuard<BTreeNode> &parent, sync::OptimisticGuard<BTreeNode> &node,
                      std::span<u8> key, std::span<const u8> payload) -> OpResult;
  auto UpdateLeafEntry(sync::OptimisticGuard<BTreeNode> &parent, sync::OptimisticGuard<BTreeNode> &node, u16 slot_id,
                       std::span<u8> key, std::span<const u8> payload, const AccessPayloadFunc &func) -> OpResult;

  /* Access record utility for scan */
  auto AccessRecord(sync::OptimisticGuard<BTreeNode> &node, u64 pos, const AccessRecordFunc &fn) -> OpResult;

  /* Core properties */
  buffer::BufferManager *buffer_;
  leng_t metadata_slotid_;
  pageid_t metadata_pid_;
  bool system_;

  /* Comparison properties */
  ComparisonLambda cmp_lambda_{ComparisonOperator::MEMCMP, std::memcmp};
};

}  // namespace leanstore::storage