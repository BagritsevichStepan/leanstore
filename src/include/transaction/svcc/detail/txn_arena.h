#pragma once

#include "common/typedefs.h"

#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace leanstore::transaction::svcc::detail {

/**
 * Bump allocator of a worker for the data of its running transaction, e.g. the copies of the locked keys.
 *
 * Nothing is freed one by one: Reset() at the end of the transaction frees everything at once. The blocks are kept
 * for the next transactions (up to MAX_IDLE_BLOCKS), an allocation never moves earlier ones.
 */
class TxnArena {
 public:
  auto Allocate(size_t size, size_t alignment) -> u8 *;
  /* A copy of bytes in the arena */
  auto Copy(std::span<const u8> bytes) -> std::span<const u8>;

  void Reset();

 private:
  static constexpr size_t BYTES_PER_BLOCK = 64 * 1024;
  static constexpr size_t MAX_IDLE_BLOCKS = 16;

  std::vector<std::unique_ptr<u8[]>> blocks_;
  std::vector<std::unique_ptr<u8[]>> large_blocks_;  // allocations of more than BYTES_PER_BLOCK, one each
  size_t block_index_ = 0;
  size_t offset_ = 0;
};

// Implementation
inline auto TxnArena::Allocate(size_t size, size_t alignment) -> u8 * {
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

inline auto TxnArena::Copy(std::span<const u8> bytes) -> std::span<const u8> {
  if (bytes.empty()) {
    return {};
  }

  auto copy = Allocate(bytes.size(), 1);
  std::memcpy(copy, bytes.data(), bytes.size());

  return {copy, bytes.size()};
}

inline void TxnArena::Reset() {
  if (blocks_.size() > MAX_IDLE_BLOCKS) {
    blocks_.resize(MAX_IDLE_BLOCKS);
  }
  large_blocks_.clear();
  block_index_ = 0;
  offset_ = 0;
}

}  // namespace leanstore::transaction::svcc::detail
