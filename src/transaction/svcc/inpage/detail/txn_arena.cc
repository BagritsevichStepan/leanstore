#include "transaction/svcc/inpage/detail/txn_arena.h"

namespace leanstore::transaction::svcc::inpage::detail {

auto TxnArena::Allocate(size_t size, size_t alignment) -> u8 * {
  if (size > BYTES_PER_BLOCK) {
    large_blocks_.push_back(std::make_unique_for_overwrite<u8[]>(size));
    return large_blocks_.back().get();
  }

  auto offset = (offset_ + alignment - 1) & ~(alignment - 1);
  if (blocks_.empty() || offset + size > BYTES_PER_BLOCK) {
    if (!blocks_.empty()) {
      block_index_++;
    }

    if (block_index_ == blocks_.size()) {
      blocks_.push_back(std::make_unique_for_overwrite<u8[]>(BYTES_PER_BLOCK));
    }

    offset = 0;
  }

  offset_ = offset + size;
  return blocks_[block_index_].get() + offset;
}

}  // namespace leanstore::transaction::svcc::inpage::detail
