#include "storage/btree/tree.h"
#include "common/constants.h"
#include "common/exceptions.h"
#include "common/utils.h"
#include "leanstore/env.h"
#include "storage/blob/blob_manager.h"
#include "transaction/svcc/evicted_lock_store.h"
#include "transaction/svcc/page_lock_state.h"
#include "transaction/transaction.h"

#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

using leanstore::sync::DeferLog;
using leanstore::sync::ExclusiveGuard;
using leanstore::sync::OptimisticGuard;
using leanstore::sync::SharedGuard;
using leanstore::transaction::Transaction;

namespace leanstore::storage {

BTree::BTree(buffer::BufferManager *buffer_pool, u32 tree_slot, bool system)
    : buffer_(buffer_pool), metadata_slotid_(tree_slot), metadata_pid_(METADATA_PAGE_ID), system_(system) {
  if (system_) {
    ExclusiveGuard<MetadataPage> meta_page(buffer_, buffer_->AllocPage(true));
    metadata_pid_ = meta_page.PageID();
    InitRoot(meta_page);
  } else if (!FLAGS_wal_enable_recovery) {
    ExclusiveGuard<MetadataPage> meta_page(buffer_, METADATA_PAGE_ID);
    InitRoot(meta_page);
  }
}

void BTree::InitRoot(ExclusiveGuard<MetadataPage> &meta_page) {
  ExclusiveGuard<BTreeNode> root_page(buffer_, buffer_->AllocPage(system_));
  new (root_page.Ptr()) storage::BTreeNode(true);
  buffer_->InitLockState(root_page.PageID());
  meta_page->roots[metadata_slotid_] = root_page.PageID();
  // -------------------------------------------------------------------------------------
  if (IsWalEnabled()) {
    GenerateWALNewRoot(meta_page, metadata_slotid_, root_page.PageID());
    GenerateWALFreshPage(root_page, true);
  }
}

auto BTree::IsWalEnabled() const -> bool {
  return FLAGS_wal_enable && !system_;
}

auto BTree::IsMvcc() const -> bool {
  return !system_ && transaction::IsMvcc();
}

auto BTree::IsSvccLockTable() const -> bool {
  return !system_ && transaction::IsSvccLockTable();
}

auto BTree::IsSvccInPageLock() const -> bool {
  return !system_ && transaction::IsSvccInPageLock();
}

void BTree::SetComparisonOperator(ComparisonLambda cmp_op) { cmp_lambda_ = cmp_op; }

auto BTree::IterateAllNodes(OptimisticGuard<BTreeNode> &node, const std::function<u64(BTreeNode &)> &inner_fn,
                            const std::function<u64(BTreeNode &)> &leaf_fn) -> u64 {
  if (!node->IsInner()) { return leaf_fn(*(node.Ptr())); }

  u64 res = inner_fn(*(node.Ptr()));
  for (auto idx = 0; idx < node->header.count; idx++) {
    auto child_pid = node->GetChild(idx);
    OptimisticGuard<BTreeNode> child(buffer_, child_pid);
    res += IterateAllNodes(child, inner_fn, leaf_fn);
  }
  OptimisticGuard<BTreeNode> child(buffer_, node->header.right_most_child);
  res += IterateAllNodes(child, inner_fn, leaf_fn);
  return res;
}

auto BTree::IterateUntils(OptimisticGuard<BTreeNode> &node, const std::function<bool(BTreeNode &)> &inner_fn,
                          const std::function<bool(BTreeNode &)> &leaf_fn) -> bool {
  if (!node->IsInner()) { return leaf_fn(*(node.Ptr())); }

  auto res = inner_fn(*(node.Ptr()));
  if (!res) {
    for (auto idx = 0; idx < node->header.count; idx++) {
      OptimisticGuard<BTreeNode> child(buffer_, node->GetChild(idx));
      res |= IterateAllNodes(child, inner_fn, leaf_fn);
      if (res) { break; }
    }
    if (!res) {
      OptimisticGuard<BTreeNode> child(buffer_, node->header.right_most_child);
      res |= IterateAllNodes(child, inner_fn, leaf_fn);
    }
  }

  return res;
}

// -------------------------------------------------------------------------------------
auto BTree::FindLeafOptimisticWithParent(std::span<u8> key)
  -> std::pair<sync::OptimisticGuard<BTreeNode>, sync::OptimisticGuard<BTreeNode>> {
  OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
  OptimisticGuard<BTreeNode> node(buffer_, reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_),
                                  parent);

  while (node->IsInner()) {
    parent = std::move(node);
    node   = OptimisticGuard<BTreeNode>(buffer_, parent->FindChild(key, cmp_lambda_), parent);
  }

  return {std::move(parent), std::move(node)};
}

auto BTree::FindLeafOptimistic(std::span<u8> key) -> OptimisticGuard<BTreeNode> {
  OptimisticGuard<MetadataPage> meta(buffer_, metadata_pid_);
  auto root_pid = meta->GetRoot(metadata_slotid_);
  OptimisticGuard<BTreeNode> node(buffer_, root_pid, meta);

  while (node->IsInner()) {
    auto next_pid = node->FindChild(key, cmp_lambda_);
    node          = OptimisticGuard<BTreeNode>(buffer_, next_pid, node);
  }
  return node;
}

void BTree::TrySplit(ExclusiveGuard<BTreeNode> &&parent, ExclusiveGuard<BTreeNode> &&node) {
  // create new root if necessary
  if (parent.PageID() == metadata_pid_) {
    auto meta_p = reinterpret_cast<MetadataPage *>(parent.Ptr());
    // Root node is full, alloc a new root
    ExclusiveGuard<BTreeNode> new_root(buffer_, buffer_->AllocPage(system_));
    new (new_root.Ptr()) storage::BTreeNode(false);
    new_root->header.right_most_child = node.PageID();
    if (IsWalEnabled()) {
      GenerateWALNewRoot(parent, metadata_slotid_, new_root.PageID());
      GenerateWALFreshPage(new_root, false, node.PageID());
    }
    // Update root pid
    meta_p->roots[metadata_slotid_] = new_root.PageID();
    parent                          = std::move(new_root);
  }

  // split & retrieve new separator
  assert(parent->IsInner());
  auto sep_info = node->FindSeparator(cmp_lambda_);
  u8 sep_key[sep_info.len];
  node->GetSeparatorKey(sep_key, sep_info);

  if (parent->HasSpaceForKV(sep_info.len, sizeof(pageid_t))) {
    // A split never changes the kind of the node
    const bool node_is_inner = node->IsInner();

    // alloc a new child page
    ExclusiveGuard<BTreeNode> new_child(buffer_, buffer_->AllocPage(system_));
    new (new_child.Ptr()) storage::BTreeNode(!node_is_inner);

    if (!node_is_inner) {
      buffer_->InitLockState(new_child.PageID());
    }

    // now split the node
    node->SplitNode(parent.Ptr(), new_child.Ptr(), node.PageID(), new_child.PageID(), sep_info.slot,
                    {sep_key, sep_info.len}, cmp_lambda_);
    assert(node->IsInner() == new_child->IsInner());

    // SVCC in-page: the locks of the keys that moved to the new leaf move along
    if (IsSvccInPageLock() && !node_is_inner) {
      auto left_locks = buffer_->BufferFramePage(node.PageID()).lock_state->Lock();
      auto right_locks = buffer_->BufferFramePage(new_child.PageID()).lock_state->Lock();
      auto left_table = left_locks.GetLockTableIfExists();
      if (left_table != nullptr && !left_table->IsEmpty()) {
        left_table->MoveUpperRange({sep_key, sep_info.len}, cmp_lambda_, right_locks.GetOrCreateLockTable());
      }
    }
    
    // -------------------------------------------------------------------------------------
    if (IsWalEnabled()) {
      // WAL new node
      GenerateWALNewPage(new_child);
      // WAL separator to parent
      {
        auto &entry      = parent.PrepareWalEntry<WALInsertSep>(sep_info.len);
        entry.left_pid   = node.PageID();
        entry.right_pid  = new_child.PageID();
        entry.sep_length = sep_info.len;
        std::memcpy(entry.sep_key, sep_key, sep_info.len);
        parent.SubmitActiveWalEntry();
      }
      // WAL logical split
      {
        auto &entry = node.PrepareWalEntry<WALLogicalSplit>(0);
        entry.sep   = sep_info;
        if (!node_is_inner) { entry.next_leaf_node = node->header.next_leaf_node; }
        node.SubmitActiveWalEntry();
      }
    }
    return;
  }

  // must split parent to make space for separator, restart from root to do this
  node.Unlock();
  EnsureSpaceForSplit(parent.UnlockAndGetPtr(), {sep_key, sep_info.len});
}

void BTree::EnsureSpaceForSplit(BTreeNode *to_split, std::span<u8> key) {
  assert(to_split->IsInner());
  while (true) {
    try {
      OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
      OptimisticGuard<BTreeNode> node(
        buffer_, reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_), parent);

      while (node->IsInner() && (node.Ptr() != to_split)) {
        parent = std::move(node);
        node   = OptimisticGuard<BTreeNode>(buffer_, parent->FindChild(key, cmp_lambda_), parent);
      }

      if (node.Ptr() == to_split) {
        if (node->HasSpaceForKV(key.size(), sizeof(pageid_t))) {
          // someone else did split concurrently
          return;
        }

        ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        TrySplit(std::move(parent_locked), std::move(node_locked));
      }

      // complete, get out of the optimistic loop
      return;
    } catch (const sync::RestartException &) {}
  }
}

/** Try merging `left` into `right` node */
void BTree::TryMerge(ExclusiveGuard<BTreeNode> &&parent, ExclusiveGuard<BTreeNode> &&left,
                     ExclusiveGuard<BTreeNode> &&right, leng_t left_pos) {
  if (left->MergeNodes(left_pos, parent.Ptr(), right.Ptr(), cmp_lambda_)) {
    // SVCC in-page: the locks of the left leaf move to the right one. MergeNodes() leaves the left page as is
    if (IsSvccInPageLock() && !right->IsInner()) {
      auto left_locks = buffer_->BufferFramePage(left.PageID()).lock_state->Lock();
      auto right_locks = buffer_->BufferFramePage(right.PageID()).lock_state->Lock();
      auto left_table = left_locks.GetLockTableIfExists();
      if (left_table != nullptr && !left_table->IsEmpty()) {
        left_table->MergeInto(right_locks.GetOrCreateLockTable());
      }
      left_locks.Retire();
    }
    // TODO(XXX): Free page left.PageID()
    if (IsWalEnabled()) {
      // WAL parent remove slot
      {
        auto &entry   = parent.PrepareWalEntry<WALRemove>(0);
        entry.slot_id = left_pos;
        parent.SubmitActiveWalEntry();
      }
      // WAL new page -- it is simpler to implement, but probably slightly inefficient
      GenerateWALNewPage(right);
    }
    if (parent->FreeSpaceAfterCompaction() >= BTreeNodeHeader::SIZE_UNDER_FULL) {
      left.Unlock();
      right.Unlock();
      // Parent node is underfull, try merge this inner node
      EnsureUnderfullInnersForMerge(parent.UnlockAndGetPtr());
    }
  }
}

void BTree::EnsureUnderfullInnersForMerge(BTreeNode *to_merge) {
  assert(to_merge->IsInner());
  auto rep_key = to_merge->GetUpperFence();
  while (true) {
    try {
      // TODO(XXX): Implement tree-level compression
      //  i.e. parent of parent is page 0 (i.e. metadata page) and we can compress the inner nodes
      OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
      OptimisticGuard<BTreeNode> node(
        buffer_, reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_), parent);

      leng_t node_pos = 0;
      while (node->IsInner() && (node.Ptr() != to_merge)) {
        parent = std::move(node);
        node   = OptimisticGuard<BTreeNode>(buffer_, parent->FindChild(rep_key, node_pos, cmp_lambda_), parent);
      }

      if (parent.PageID() != metadata_pid_ &&  // Root node can't be merged
          node.Ptr() == to_merge &&               // Found the correct node to be merged
          node_pos < parent->header.count &&      // Current node is not the right most child
          parent->header.count >= 1 &&            // Parent has more than one children
          (node->FreeSpaceAfterCompaction() >= BTreeNodeHeader::SIZE_UNDER_FULL)  // Current node is underfull
      ) {
        // underfull
        auto right_pid =
          (node_pos < parent->header.count - 1) ? parent->GetChild(node_pos + 1) : parent->header.right_most_child;
        OptimisticGuard<BTreeNode> right(buffer_, right_pid, parent);
        if (right->FreeSpaceAfterCompaction() >= (PAGE_SIZE - BTreeNodeHeader::SIZE_UNDER_FULL)) {
          ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
          ExclusiveGuard<BTreeNode> node_locked(std::move(node));
          ExclusiveGuard<BTreeNode> right_locked(std::move(right));
          TryMerge(std::move(parent_locked), std::move(node_locked), std::move(right_locked), node_pos);
        }
      }
      // complete, get out of the optimistic loop
      return;
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::InsertIntoLeaf(OptimisticGuard<BTreeNode> &parent, OptimisticGuard<BTreeNode> &node, std::span<u8> key,
                           std::span<const u8> payload) -> OpResult {
  if (!node->HasSpaceForKV(key.size(), payload.size())) { return OpResult::NEED_SPLIT; }

  /* Check if current key exists or not */
  bool found;
  auto pos = node->LowerBound(key, found, cmp_lambda_);
  if (found) { return OpResult::DUPLICATE; }

  /* Concurrency control */
  if (IsSvccLockTable() && !node.TryLock(metadata_slotid_, 0UL, {}, key)) { return OpResult::ABORT_TX; }

  /* Leaf node has enough space -> only latch leaf node */
  auto defer_log = DeferLog<BTreeNode>(); /* Alternative WAL cycle - automatically append log when the scope ends */
  {
    ExclusiveGuard<BTreeNode> node_locked(std::move(node));
    parent.ValidateOrRestart();

    /* SVCC in-page: lock before inserting, a reader may hold a lock on this key while it is still absent */
    if ((IsMvcc() || IsSvccInPageLock()) && !node_locked.TryLock(metadata_slotid_, 0UL, {}, key)) { return OpResult::ABORT_TX; }

    pos = node_locked->InsertKeyValue(key, payload, cmp_lambda_);
    if (IsMvcc()) { node_locked->UpdateTimestamp(pos, transaction::Transaction::TUPLE_UNDO_TIMESTAMP); }

    if (IsWalEnabled()) { defer_log.Construct<WALInsert>(node_locked, key, payload); }
  }

  return OpResult::OK;
}

auto BTree::UpdateLeafEntry(OptimisticGuard<BTreeNode> &parent, OptimisticGuard<BTreeNode> &node, u16 slot_id,
                            std::span<u8> key, std::span<const u8> payload, const AccessPayloadFunc &func) -> OpResult {
  auto curr_payload = node->GetPayload(slot_id);
  if (payload.size() > curr_payload.size() && !node->HasSpaceForKV(key.size(), payload.size() - curr_payload.size())) {
    return OpResult::NEED_SPLIT;
  }
  // SVCC
  auto tuple_ts = node->GetTimestamp(slot_id);
  node.ValidateOrRestart(false);
  if (IsSvccLockTable() && !node.TryLock(metadata_slotid_, tuple_ts, curr_payload, key)) { return OpResult::ABORT_TX; }
  // only lock leaf
  ExclusiveGuard<BTreeNode> node_locked(std::move(node));
  parent.ValidateOrRestart();
  // MVCC or SVCC with in-page lock
  if ((IsMvcc() || IsSvccInPageLock()) && !node_locked.TryLock(metadata_slotid_, node_locked->GetTimestamp(slot_id),
                                                 node_locked->GetPayload(slot_id), key)) {
    return OpResult::ABORT_TX;
  }
  // Log previous payload, trigger func utility if provided, and remove the entry
  if (IsWalEnabled()) {
    auto &entry   = node_locked.PrepareWalEntry<WALRemove>(0);
    entry.slot_id = slot_id;
    node_locked.SubmitActiveWalEntry();
  }
  if (func) { func(curr_payload); }
  // Update key-value
  node_locked->RemoveSlot(slot_id, false);
  slot_id = node_locked->InsertKeyValue(key, payload, cmp_lambda_);
  if (IsMvcc()) { node_locked->UpdateTimestamp(slot_id, transaction::Transaction::TUPLE_UNDO_TIMESTAMP); }

  if (IsWalEnabled()) { GenerateWAL<ExclusiveGuard<BTreeNode>, WALInsert>(node_locked, key, payload); }
  // --------------------------------------------------------------------------
  return OpResult::OK;  // success
}

// -------------------------------------------------------------------------------------
auto BTree::LookUp(std::span<u8> key, const AccessPayloadFunc &read_cb) -> OpResult {
  auto &txn                      = Transaction::active_txn;
  const bool needs_version_chain = IsMvcc() && txn.iso_level >= transaction::IsolationLevel::SNAPSHOT_ISOLATION;

  while (true) {
    try {
      OptimisticGuard<BTreeNode> node = FindLeafOptimistic(key);
      if (!system_ && !node.TryLockShared(metadata_slotid_, key)) { return OpResult::ABORT_TX; }
      bool found;
      leng_t pos = node->LowerBound(key, found, cmp_lambda_);

      // Own write is always visible regardless of isolation level
      if (IsMvcc() && node.OwnTuple(metadata_slotid_, key)) {
        Ensure(found);
        read_cb(node->GetPayload(pos));
        return OpResult::OK;
      }

      // MVCC: check if we need to access version chain
      if (needs_version_chain) {
        LOCKABLE_TUPLE_STACK(lockable, key, metadata_slotid_);
        auto tuple_ts = found ? node->GetTimestamp(pos) : transaction::INVALID_TS;
        node.ValidateOrRestart(false);

        // If key missing, it means that has never been existed
        if (!found) { return OpResult::NOT_FOUND; }

        // If current version too new for our snapshot -- walk version chain
        if (txn.start_ts < tuple_ts) {
          auto read_success = txn.LookupVersionChain(lockable, read_cb, tuple_ts);
          txn.UpdateTupleReadTS(lockable, tuple_ts);
          return read_success ? OpResult::OK : OpResult::NOT_FOUND;
        }

        // Current version visible -- record read TS for validation, then fall through
        txn.UpdateTupleReadTS(lockable, tuple_ts);
      } else if (!found) {
        return OpResult::NOT_FOUND;
      }

      // Happy path
      auto payload = node->GetPayload(pos);
      if (IsMvcc() && payload.empty()) { return OpResult::NOT_FOUND; }
      read_cb(payload);
      return OpResult::OK;
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::Insert(std::span<u8> key, std::span<const u8> payload) -> OpResult {
  assert((key.size() + payload.size()) <= BTreeNode::MAX_RECORD_SIZE);

  while (true) {
    try {
      auto [parent, node] = FindLeafOptimisticWithParent(key);

      // Found the leaf node to insert new data
      auto op_ret = InsertIntoLeaf(parent, node, key, payload);
      if (op_ret != OpResult::NEED_SPLIT) { return op_ret; }

      // The leaf node doesn't have enough space, we have to split it
      ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
      ExclusiveGuard<BTreeNode> node_locked(std::move(node));
      TrySplit(std::move(parent_locked), std::move(node_locked));

      // We haven't run the insertion yet, so we run the loop again to insert the record
    } catch (const sync::RestartException &) {}
  }
}

// This API is mostly used for UNDO during transaction abort
// It has many duplicated code with Insert/Update -- we should refactor it later
auto BTree::Upsert(std::span<u8> key, std::span<const u8> payload) -> OpResult {
  assert((key.size() + payload.size()) <= BTreeNode::MAX_RECORD_SIZE);

  while (true) {
    try {
      auto [parent, node] = FindLeafOptimisticWithParent(key);

      // Lookup key
      bool found;
      auto slot_id = node->LowerBound(key, found, cmp_lambda_);
      if (!found) {
        // Try to insert the key-value
        auto op_ret = InsertIntoLeaf(parent, node, key, payload);
        if (op_ret != OpResult::NEED_SPLIT) { return op_ret; }
      } else {
        auto op_ret = UpdateLeafEntry(parent, node, slot_id, key, payload, {});
        if (op_ret != OpResult::NEED_SPLIT) { return op_ret; }
      }

      // The leaf node doesn't have enough space, we have to split it
      ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
      ExclusiveGuard<BTreeNode> node_locked(std::move(node));
      TrySplit(std::move(parent_locked), std::move(node_locked));

      // We haven't run the insertion yet, so we run the loop again to insert the record
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::Remove(std::span<u8> key) -> OpResult {
  while (true) {
    try {
      OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
      OptimisticGuard<BTreeNode> node(
        buffer_, reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_), parent);

      leng_t node_pos = 0;
      while (node->IsInner()) {
        parent = std::move(node);
        node   = OptimisticGuard<BTreeNode>(buffer_, parent->FindChild(key, node_pos, cmp_lambda_), parent);
      }

      // Lookup key
      bool found;
      auto slot_id = node->LowerBound(key, found, cmp_lambda_);
      // TODO(XXX): Should we return OpResult::ABORT_TX here if an uncommited txn deleted the key?
      if (!found) { return OpResult::NOT_FOUND; }

      // Concurrency control
      auto tuple_ts = node->GetTimestamp(slot_id);
      auto payload  = node->GetPayload(slot_id);
      node.ValidateOrRestart(false);
      if (IsSvccLockTable() && !node.TryLock(metadata_slotid_, tuple_ts, payload, key)) { return OpResult::ABORT_TX; }

      // Removal
      leng_t entry_size = node->slots[slot_id].key_length + payload.size();
      if ((node->FreeSpaceAfterCompaction() + entry_size >=
           BTreeNodeHeader::SIZE_UNDER_FULL) &&     // new node is under full
          (parent.PageID() != metadata_pid_) &&  // current node is not the root node
          (parent->header.count >= 2) &&            // parent has more than one children
          ((node_pos + 1) < parent->header.count)   // current node has a right sibling
      ) {
        // underfull
        ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        ExclusiveGuard<BTreeNode> right_locked(buffer_, parent_locked->GetChild(node_pos + 1));

        // Update by remove then insert
        if ((IsMvcc() || IsSvccInPageLock()) && !node_locked.TryLock(metadata_slotid_, node_locked->GetTimestamp(slot_id),
                                                       node_locked->GetPayload(slot_id), key)) {
          return OpResult::ABORT_TX;
        }
        node_locked->RemoveSlot(slot_id, IsMvcc());
        if (IsMvcc()) { node_locked->UpdateTimestamp(slot_id, transaction::Transaction::TUPLE_UNDO_TIMESTAMP); }
        // --------------------------------------------------------------------------
        // WAL Remove
        if (IsWalEnabled()) {
          auto &entry   = node_locked.PrepareWalEntry<WALRemove>(0);
          entry.slot_id = slot_id;
          node_locked.SubmitActiveWalEntry();
        }
        // --------------------------------------------------------------------------
        // right child is also under full
        if (right_locked->FreeSpaceAfterCompaction() >= (PAGE_SIZE - BTreeNodeHeader::SIZE_UNDER_FULL)) {
          TryMerge(std::move(parent_locked), std::move(node_locked), std::move(right_locked), node_pos);
        }
      } else {
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        parent.ValidateOrRestart();
        // Update by remove then insert
        if ((IsMvcc() || IsSvccInPageLock()) && !node_locked.TryLock(metadata_slotid_, node_locked->GetTimestamp(slot_id),
                                                       node_locked->GetPayload(slot_id), key)) {
          return OpResult::ABORT_TX;
        }
        node_locked->RemoveSlot(slot_id, IsMvcc());
        if (IsMvcc()) { node_locked->UpdateTimestamp(slot_id, transaction::Transaction::TUPLE_UNDO_TIMESTAMP); }
        // --------------------------------------------------------------------------
        // WAL Remove
        if (IsWalEnabled()) {
          auto &entry   = node_locked.PrepareWalEntry<WALRemove>(0);
          entry.slot_id = slot_id;
          node_locked.SubmitActiveWalEntry();
        }
        // --------------------------------------------------------------------------
      }
      return OpResult::OK;
    } catch (const sync::RestartException &) {}
  }
}

/**
 * @brief Atomically update the key-payload pair
 * Return true/false whether the key exists and is updated successfully
 *
 * If `func` is provided, then func(previous payload) is triggered
 */
auto BTree::Update(std::span<u8> key, std::span<const u8> payload, const AccessPayloadFunc &func) -> OpResult {
  assert((key.size() + payload.size()) <= BTreeNode::MAX_RECORD_SIZE);

  while (true) {
    try {
      auto [parent, node] = FindLeafOptimisticWithParent(key);

      // Lookup key
      bool found;
      auto slot_id = node->LowerBound(key, found, cmp_lambda_);
      if (!found) { return OpResult::NOT_FOUND; }

      // Try update
      auto op_ret = UpdateLeafEntry(parent, node, slot_id, key, payload, func);
      if (op_ret != OpResult::NEED_SPLIT) { return op_ret; }

      // The leaf node doesn't have enough space, we have to split it
      ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
      ExclusiveGuard<BTreeNode> node_locked(std::move(node));
      TrySplit(std::move(parent_locked), std::move(node_locked));

      // We haven't run the insertion yet, so we run the loop again to insert the record
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::UpdateInPlace(std::span<u8> key, const ModifyPayloadFunc &func, FixedSizeDelta *delta) -> OpResult {
  while (true) {
    try {
      auto node = FindLeafOptimistic(key);
      bool found;
      auto pos = node->LowerBound(key, found, cmp_lambda_);
      if (!found) { return OpResult::NOT_FOUND; }

      /* Concurrency control */
      auto tuple_ts = node->GetTimestamp(pos);
      node.ValidateOrRestart(false);
      if (IsSvccLockTable() && !node.TryLock(metadata_slotid_, tuple_ts, node->GetPayload(pos), key)) {
        return OpResult::ABORT_TX;
      }

      /* Alternative WAL cycle - automatically append WAL entry when the scope ends */
      auto defer_log = DeferLog<BTreeNode>();
      {
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        auto payload = node_locked->GetPayload(pos);
        if ((IsMvcc() || IsSvccInPageLock()) && !node_locked.TryLock(metadata_slotid_, node_locked->GetTimestamp(pos), payload, key)) {
          return OpResult::ABORT_TX;
        }

        /* Modify the record, and store the after-value */
        func(payload);
        if (IsMvcc()) { node_locked->UpdateTimestamp(pos, transaction::Transaction::TUPLE_UNDO_TIMESTAMP); }
        if (IsWalEnabled() && delta != nullptr) { delta->UpdateDeltaPayload(payload); }

        /* Generate the delta record */
        if (IsWalEnabled()) {
          if (delta != nullptr) {
            defer_log.Construct<WALDeltaImage>(node_locked, key,
                                               std::span<u8>(reinterpret_cast<u8 *>(delta), delta->size));
          } else {
            defer_log.Construct<WALAfterImage>(node_locked, key, payload);
          }
        }
      }

      return OpResult::OK;
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::ScanAscending(std::span<u8> key, const AccessRecordFunc &fn) -> OpResult {
  while (true) {
    try {
      auto node = FindLeafOptimistic(key);
      bool unused;
      auto pos = node->LowerBound(key, unused, cmp_lambda_);
      while (true) {
        if (pos < node->header.count) {
          auto op_ret = AccessRecord(node, pos, fn);
          if (op_ret == OpResult::ABORT_TX) { return op_ret; }
          if (op_ret == OpResult::STOP_SCAN) { return OpResult::OK; }
          pos++;
        } else {
          // The next leaf is found by a descent, not by next_leaf_node: a merge doesn't update the left neighbor
          // of the leaf merged away, which then still points to that dead leaf
          if (node->header.upper_fence.len == 0) {
            return OpResult::OK;
          }

          // The smallest key greater than the upper fence (inclusive) is in the next leaf
          auto upper_fence = node->GetUpperFence();
          std::vector<u8> next_key(upper_fence.begin(), upper_fence.end());
          next_key.push_back(0);

          node = FindLeafOptimistic(next_key);
          pos = node->LowerBound(next_key, unused, cmp_lambda_);
        }
      }
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::ExtractAscending(std::span<u8> key, const KeyPredicateFunc &matches, const AccessRecordFunc &consume)
  -> OpResult {
  std::vector<u8> from(key.begin(), key.end());

  u8 record_key[BTreeNode::MAX_RECORD_SIZE];
  while (true) {
    try {
      OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
      OptimisticGuard<BTreeNode> node(
        buffer_, reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_), parent);

      leng_t node_pos = 0;
      while (node->IsInner()) {
        parent = std::move(node);
        node   = OptimisticGuard<BTreeNode>(buffer_, parent->FindChild(from, node_pos, cmp_lambda_), parent);
      }

      // The matching records are consecutive from the lower bound. They are counted on the optimistic read: the
      // upgrade to the exclusive latch below restarts if the leaf changed since, otherwise the count holds
      const leng_t first_pos = node->LowerBound(from, cmp_lambda_);
      leng_t final_pos = first_pos;
      u64 bytes_to_remove = 0;

      while (final_pos < node->header.count) {
        const leng_t prefix_len = node->header.prefix_len;
        const leng_t slot_key_len = node->slots[final_pos].key_length;
        const u32 key_len = prefix_len + slot_key_len;

        if (key_len > BTreeNode::MAX_RECORD_SIZE) {
          node.ValidateOrRestart(false);
          UnreachableCode();
        }

        std::memcpy(record_key, node->GetPrefix(), prefix_len);
        std::memcpy(record_key + prefix_len, node->GetKey(final_pos), slot_key_len);

        node.ValidateOrRestart(false);

        if (!matches({record_key, key_len})) {
          break;
        }

        bytes_to_remove += node->slots[final_pos].key_length + node->GetPayload(final_pos).size();
        final_pos++;
      }

      const bool is_range_end_in_current_leaf = final_pos < node->header.count;
      if (first_pos == final_pos && is_range_end_in_current_leaf) {
        node.ValidateOrRestart(false);
        return OpResult::OK;
      }

      auto remove_matching_cb = [&](ExclusiveGuard<BTreeNode> &leaf) -> OpResult {
        leng_t pos = first_pos;

        for (leng_t removed = 0; removed < final_pos - first_pos; removed++) {
          const u32 key_len = leaf->header.prefix_len + leaf->slots[pos].key_length;
          std::memcpy(record_key, leaf->GetPrefix(), leaf->header.prefix_len);
          std::memcpy(record_key + leaf->header.prefix_len, leaf->GetKey(pos), leaf->slots[pos].key_length);
          auto key_span = std::span<u8>{record_key, key_len};

          if (!system_ && !leaf.TryLock(metadata_slotid_, leaf->GetTimestamp(pos), leaf->GetPayload(pos), key_span)) {
            return OpResult::ABORT_TX;
          }

          consume(key_span, leaf->GetPayload(pos));
          leaf->RemoveSlot(pos, IsMvcc());

          if (IsWalEnabled()) {
            auto &entry = leaf.PrepareWalEntry<WALRemove>(0);
            entry.slot_id = pos;
            leaf.SubmitActiveWalEntry();
          }

          if (IsMvcc()) {
            leaf->UpdateTimestamp(pos, transaction::Transaction::TUPLE_UNDO_TIMESTAMP);
            pos++;
          }
        }

        if (!is_range_end_in_current_leaf && leaf->header.upper_fence.len > 0) {
          auto upper_fence = leaf->GetUpperFence();
          from.assign(upper_fence.begin(), upper_fence.end());
          from.push_back(0);
        }

        return OpResult::OK;
      };

      const bool is_last_leaf = node->header.upper_fence.len == 0;
      OpResult operation_result;

      if ((first_pos < final_pos) &&
          (node->FreeSpaceAfterCompaction() + bytes_to_remove >= BTreeNodeHeader::SIZE_UNDER_FULL) &&
          (parent.PageID() != metadata_pid_) &&
          (parent->header.count >= 2) &&
          ((node_pos + 1) < parent->header.count)
      ) {
        // underfull
        ExclusiveGuard<BTreeNode> parent_locked(std::move(parent));
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        ExclusiveGuard<BTreeNode> right_locked(buffer_, parent_locked->GetChild(node_pos + 1));

        operation_result = remove_matching_cb(node_locked);

        if (operation_result == OpResult::OK &&
            right_locked->FreeSpaceAfterCompaction() >= (PAGE_SIZE - BTreeNodeHeader::SIZE_UNDER_FULL)) {
          TryMerge(std::move(parent_locked), std::move(node_locked), std::move(right_locked), node_pos);
        }
      } else {
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        parent.ValidateOrRestart();

        operation_result = remove_matching_cb(node_locked);
      }

      if (operation_result != OpResult::OK || is_range_end_in_current_leaf || is_last_leaf) {
        return operation_result;
      }
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::ScanDescending(std::span<u8> key, const AccessRecordFunc &fn) -> OpResult {
  while (true) {
    try {
      auto node = FindLeafOptimistic(key);
      bool found;
      int pos = static_cast<int>(node->LowerBound(key, found, cmp_lambda_));
      // LowerBound search always return the first position whose key >= the search key
      // hence, if LowerBound doesn't give an exact match, the found key will > search key as we scan desc,
      // any key > search key should be overlooked, i.e. start from pos - 1
      if (!found) { pos--; }
      while (true) {
        while (pos >= 0) {
          if (pos < node->header.count) {
            auto op_ret = AccessRecord(node, pos, fn);
            if (op_ret == OpResult::ABORT_TX) { return op_ret; }
            if (op_ret == OpResult::STOP_SCAN) { return OpResult::OK; }
          }
          pos--;
        }
        if (node->header.IsLowerFenceInfinity()) { return OpResult::OK; }
        node = FindLeafOptimistic(node->GetLowerFence());
        pos  = node->header.count - 1;
      }
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::CountEntries() -> u64 {
  OptimisticGuard<MetadataPage> meta(buffer_, metadata_pid_);
  auto root_pid = meta->GetRoot(metadata_slotid_);
  OptimisticGuard<BTreeNode> node(buffer_, root_pid, meta);

  return IterateAllNodes(
    node, [](BTreeNode &) { return 0; },
    [this](BTreeNode &node) {
      auto ret = node.header.count;
      for (auto idx = 0; idx < node.header.count; idx++) {
        if (IsMvcc()) { assert(node.GetTimestamp(idx) != transaction::INVALID_TS); }
        if (node.slots[idx].payload_length == 0) { ret--; }
      }
      return ret;
    });
}

// -------------------------------------------------------------------------------------

auto BTree::IsNotEmpty() -> bool {
  OptimisticGuard<MetadataPage> meta(buffer_, metadata_pid_);
  OptimisticGuard<BTreeNode> node(buffer_, meta->GetRoot(metadata_slotid_), meta);

  return IterateUntils(
    node, [](BTreeNode &) { return false; }, [](BTreeNode &node) { return (node.header.count > 0); });
}

auto BTree::CountPages() -> u64 {
  OptimisticGuard<MetadataPage> meta(buffer_, metadata_pid_);
  OptimisticGuard<BTreeNode> node(buffer_, meta->GetRoot(metadata_slotid_), meta);

  return IterateAllNodes(node, [](BTreeNode &) { return 1; }, [](BTreeNode &) { return 1; });
}

auto BTree::GetTimestamp(std::span<u8> key) -> timestamp_t {
  while (true) {
    try {
      auto node = FindLeafOptimistic(key);
      bool found;
      auto pos = node->LowerBound(key, found, cmp_lambda_);
      if (!found) { return transaction::INVALID_TS; }
      return node->GetTimestamp(pos);
    } catch (const sync::RestartException &) {}
  }
}

/* Transaction commit will call this function to update tuples' timestamp */
void BTree::UpdateTimestamp(std::span<u8> key, timestamp_t commit_ts) {
  while (true) {
    try {
      auto node = FindLeafOptimistic(key);
      bool found;
      auto pos = node->LowerBound(key, found, cmp_lambda_);
      if (found) {
        ExclusiveGuard<BTreeNode> node_locked(std::move(node));
        assert(node_locked->GetTimestamp(pos) == transaction::INVALID_TS);
        node_locked->UpdateTimestamp(pos, commit_ts);
      }
      return;
    } catch (const sync::RestartException &) {}
  }
}

void BTree::ReleaseInPageLock(std::span<const u8> key, pageid_t leaf_hint, bool exclusive) {
  // 1. Fast path: the lock is still in the table of the hint leaf. A held lock is in exactly one table,
  // so finding the key there means it is ours. Absent => split/merge/eviction moved it away
  auto hint_lock_state = buffer_->BufferFramePage(leaf_hint).lock_state.get();
  Ensure(hint_lock_state != nullptr);
  
  {
    auto guard = hint_lock_state->Lock();
    auto table = guard.IsInMemory() ? guard.GetLockTableIfExists() : nullptr;
    if (table != nullptr && table->TryRelease(key, exclusive)) {
      return;
    }
  }

  // 2. Slow path: descend by key
  while (true) {
    try {
      OptimisticGuard<BTreeNode> parent(buffer_, metadata_pid_);
      auto child = reinterpret_cast<MetadataPage *>(parent.Ptr())->GetRoot(metadata_slotid_);
      while (true) {
        // child was read from a consistent parent
        parent.ValidateOrRestart(false);

        auto lock_state = buffer_->BufferFramePage(child).lock_state.get();
        if (lock_state == nullptr) {
          // An inner node
          OptimisticGuard<BTreeNode> node(buffer_, child, parent);
          child = node->FindChild(key, cmp_lambda_);
          parent = std::move(node);
          continue;
        }

        // A leaf: unchanged parent => no split/merge moved the key away from it since `child` was read,
        // and they can't do it now: they need the latch of its lock state
        auto guard = lock_state->Lock();
        parent.ValidateOrRestart(true);
        if (guard.IsInMemory()) {
          guard.GetLockTable().Release(key, exclusive);
        } else {
          Ensure(guard.IsEvictedWithLocks());
          buffer_->EvictedLocks().Release(child, key, exclusive);
        }
        return;
      }
    } catch (const sync::RestartException &) {}
  }
}

auto BTree::SizeInMB() -> float { return CountPages() * static_cast<float>(PAGE_SIZE) / MB; }

/**
 * @brief Only used for Blob Handler indexes.
 * Similar to LookUp operator, but for Byte String as key
 */
auto BTree::LookUpBlob(std::span<const u8> blob_key, const ComparisonLambda &cmp, const AccessPayloadFunc &read_cb)
  -> bool {
  Ensure(cmp_lambda_.op == ComparisonOperator::BLOB_HANDLER);
  Ensure(cmp.op == ComparisonOperator::BLOB_LOOKUP);
  leng_t unused;
  auto search_key = blob::BlobLookupKey(blob_key);

  while (true) {
    try {
      OptimisticGuard<MetadataPage> meta(buffer_, metadata_pid_);
      OptimisticGuard<BTreeNode> node(buffer_, meta->GetRoot(metadata_slotid_), meta);
      while (node->IsInner()) {
        node = OptimisticGuard<BTreeNode>(buffer_, node->FindChildWithBlobKey(search_key, unused, cmp), node);
      }

      bool found;
      leng_t pos = node->LowerBoundWithBlobKey(search_key, found, cmp);
      if (!found) { return false; }

      auto payload = node->GetPayload(pos);
      read_cb(payload);
      return true;
    } catch (const sync::RestartException &) {}
  }
}

}  // namespace leanstore::storage