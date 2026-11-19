#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace leanstore::transaction::svcc::inpage::detail {

/**
 * @brief A list kept in the order of `Node::ts`, whose first node needs no allocation.
 */
template <class Node>
class TimestampSortedList {
 public:
  void Insert(Node node);
  
  void PopBack();
  void Remove(timestamp_t ts);
  
  auto Contains(timestamp_t ts) const -> bool;
  
  auto Front() const -> const Node &;
  auto Back() const -> const Node &;

  auto IsEmpty() const -> bool;
  auto Size() const -> size_t;

  template <class Fn>
  void ForEach(Fn &&fn) const;

 private:
  static_assert(std::is_trivially_copyable_v<Node>);

  void PopFront();

  /* The first node of the order, held inline so that a list of one allocates nothing */
  Node head_;
  /* The remaining nodes, in the same order, all of them after `head_` */
  std::vector<Node> tail_;
};

// Implementation
template <class Node>
void TimestampSortedList<Node>::Insert(Node node) {
  Ensure(node.ts != 0);

  if (IsEmpty()) {
    head_ = std::move(node);
    return;
  }

  if (node.ts < head_.ts) {
    tail_.insert(tail_.begin(), std::move(head_));
    head_ = std::move(node);
    return;
  }

  if (tail_.empty() || node.ts > tail_.back().ts) {
    tail_.push_back(std::move(node));
    return;
  }

  const auto it = std::upper_bound(tail_.begin(), tail_.end(), node,
                             [](const Node &lhs, const Node &rhs) { return lhs.ts < rhs.ts; });
  tail_.insert(it, std::move(node));
}

template <class Node>
void TimestampSortedList<Node>::PopFront() {
  Ensure(!IsEmpty());

  if (tail_.empty()) {
    head_ = Node();
    return;
  }

  head_ = std::move(tail_.front());
  tail_.erase(tail_.begin());
}

template <class Node>
void TimestampSortedList<Node>::Remove(timestamp_t ts) {
  Ensure(!IsEmpty());

  if (head_.ts == ts) {
    PopFront();
    return;
  }

  const auto it = std::lower_bound(tail_.begin(), tail_.end(), ts,
                                   [](const Node &node, timestamp_t ts) { return node.ts < ts; });
  Ensure(it != tail_.end() && it->ts == ts);
  tail_.erase(it);
}

template <class Node>
auto TimestampSortedList<Node>::Front() const -> const Node & {
  Ensure(!IsEmpty());
  return head_;
}

template <class Node>
auto TimestampSortedList<Node>::IsEmpty() const -> bool {
  return head_.ts == 0;
}

template <class Node>
auto TimestampSortedList<Node>::Size() const -> size_t {
  return !IsEmpty() ? 1 + tail_.size() : 0;
}

template <class Node>
auto TimestampSortedList<Node>::Contains(timestamp_t ts) const -> bool {
  if (IsEmpty()) {
    return false;
  }

  if (head_.ts == ts) {
    return true;
  }

  const auto it = std::lower_bound(tail_.begin(), tail_.end(), ts,
                                   [](const Node &node, timestamp_t ts) { return node.ts < ts; });
  return it != tail_.end() && it->ts == ts;
}

template <class Node>
void TimestampSortedList<Node>::PopBack() {
  Ensure(!IsEmpty());

  if (tail_.empty()) {
    head_ = Node();
    return;
  }

  tail_.pop_back();
}

template <class Node>
auto TimestampSortedList<Node>::Back() const -> const Node & {
  Ensure(!IsEmpty());

  if (tail_.empty()) {
    return head_;
  }

  return tail_.back();
}

template <class Node>
template <class Fn>
void TimestampSortedList<Node>::ForEach(Fn &&fn) const {
  if (IsEmpty()) {
    return;
  }

  fn(head_);
  for (const auto &node : tail_) {
    fn(node);
  }
}

}  // namespace leanstore::transaction::svcc::inpage::detail
