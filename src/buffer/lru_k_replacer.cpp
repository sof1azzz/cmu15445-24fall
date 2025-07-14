//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"

#include <random>

#include "common/exception.h"

namespace bustub {

/**
 *
 * TODO(P1): Add implementation
 *
 * @brief a new LRUKReplacer.
 * @param num_frames the maximum number of frames the LRUReplacer will be required to store
 */
LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {
  auto comparator = [k](const std::shared_ptr<LRUKNode> &a, const std::shared_ptr<LRUKNode> &b) {
    bool a_is_inf = a->history_.size() < k;
    bool b_is_inf = b->history_.size() < k;

    // 如果一个是+∞，另一个是有限值，+∞的排在前面
    if (a_is_inf && !b_is_inf) {
      return true;
    }
    if (!a_is_inf && b_is_inf) {
      return false;
    }

    // 如果都是+∞或都是有限值，按 history_.front() 排序
    if (a->history_.front() != b->history_.front()) {
      return a->history_.front() < b->history_.front();
    }

    // 如果 history_.front() 相同，按 frame_id 排序以确保一致性
    return a->fid_ < b->fid_;
  };

  node_set_ = decltype(node_set_)(comparator);
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Find the frame with largest backward k-distance and evict that frame. Only frames
 * that are marked as 'evictable' are candidates for eviction.
 *
 * A frame with less than k historical references is given +inf as its backward k-distance.
 * If multiple frames have inf backward k-distance, then evict frame whose oldest timestamp
 * is furthest in the past.
 *
 * Successful eviction of a frame should decrement the size of replacer and remove the frame's
 * access history.
 *
 * @return the frame ID if a frame is successfully evicted, or `std::nullopt` if no frames can be evicted.
 */
auto LRUKReplacer::Evict() -> std::optional<frame_id_t> {
  std::scoped_lock lock(latch_);
  for (auto it = node_set_.begin(); it != node_set_.end(); ++it) {
    const auto &node = *it;
    if (!node->is_evictable_) {
      continue;
    }
    frame_id_t fid = node->fid_;
    node_store_.erase(fid);
    node_set_.erase(it);
    curr_size_--;
    return fid;
  }
  return std::nullopt;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Record the event that the given frame id is accessed at current timestamp.
 * Create a new entry for access history if frame id has not been seen before.
 *
 * If frame id is invalid (ie. larger than replacer_size_), throw an exception. You can
 * also use BUSTUB_ASSERT to abort the process if frame id is invalid.
 *
 * @param frame_id id of frame that received a new access.
 * @param access_type type of access that was received. This parameter is only needed for
 * leaderboard tests.
 */
void LRUKReplacer::RecordAccess(frame_id_t frame_id, [[maybe_unused]] AccessType access_type) {
  BUSTUB_ASSERT(frame_id < (int)replacer_size_, "frame_id larger than replacer_size");

  std::scoped_lock lock(latch_);
  if (const auto it = node_store_.find(frame_id); it != node_store_.end()) {
    const auto node = it->second;
    // 在修改history_之前先从set中删除节点
    node_set_.erase(node);
    // 修改history_
    node->history_.emplace_back(current_timestamp_);
    if (node->history_.size() > k_) {
      node->history_.pop_front();
    }
    // 重新插入到set中
    node_set_.emplace(node);
  } else {
    auto new_node = std::make_shared<LRUKNode>(frame_id);
    new_node->history_.emplace_back(current_timestamp_);
    node_store_.emplace(frame_id, new_node);
    node_set_.emplace(new_node);
  }
  ++current_timestamp_;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Toggle whether a frame is evictable or non-evictable. This function also
 * controls replacer's size. Note that size is equal to number of evictable entries.
 *
 * If a frame was previously evictable and is to be set to non-evictable, then size should
 * decrement. If a frame was previously non-evictable and is to be set to evictable,
 * then size should increment.
 *
 * If frame id is invalid, throw an exception or abort the process.
 *
 * For other scenarios, this function should terminate without modifying anything.
 *
 * @param frame_id id of frame whose 'evictable' status will be modified
 * @param set_evictable whether the given frame is evictable or not
 */
void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock lock(latch_);

  const auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    // 对于不存在的frame，直接返回而不做任何修改
    return;
  }

  const auto node = it->second;
  if (set_evictable) {
    if (node->is_evictable_) {
      return;
    }
    node->is_evictable_ = true;
    ++curr_size_;
  } else {
    if (!node->is_evictable_) {
      return;
    }
    node->is_evictable_ = false;
    --curr_size_;
  }
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Remove an evictable frame from replacer, along with its access history.
 * This function should also decrement replacer's size if removal is successful.
 *
 * Note that this is different from evicting a frame, which always remove the frame
 * with largest backward k-distance. This function removes specified frame id,
 * no matter what its backward k-distance is.
 *
 * If Remove is called on a non-evictable frame, throw an exception or abort the
 * process.
 *
 * If specified frame is not found, directly return from this function.
 *
 * @param frame_id id of frame to be removed
 */
void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock lock(latch_);

  const auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;
  }

  const auto &node = it->second;

  BUSTUB_ASSERT(node->is_evictable_, "Attempted to remove a non-evictable frame");

  node_set_.erase(node);
  node_store_.erase(it);

  curr_size_--;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Return replacer's size, which tracks the number of evictable frames.
 *
 * @return size_t
 */
auto LRUKReplacer::Size() const -> size_t { return curr_size_; }

}  // namespace bustub
