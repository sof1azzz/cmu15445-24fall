//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_iterator.h
//
// Identification: src/include/storage/index/index_iterator.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

/**
 * index_iterator.h
 * For range scan of b+ tree
 */
#pragma once
#include <utility>
#include "buffer/buffer_pool_manager.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page_guard.h"

namespace bustub {

#define INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator>

INDEX_TEMPLATE_ARGUMENTS
class IndexIterator {
 public:
  // you may define your own constructor based on your member variables
  IndexIterator();
  IndexIterator(BufferPoolManager *bpm, ReadPageGuard read_guard, int pos = 0);
  ~IndexIterator();  // NOLINT

  using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>;
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;

  auto IsEnd() const -> bool;

  auto operator*() -> std::pair<const KeyType &, const ValueType &>;

  auto operator++() -> IndexIterator &;

  auto operator==(const IndexIterator &itr) const -> bool {
    // 两个迭代器都在末尾
    if (IsEnd() && itr.IsEnd()) {
      return true;
    }
    return read_guard_.GetPageId() == itr.read_guard_.GetPageId() && pos_ == itr.pos_;
  }

  auto operator!=(const IndexIterator &itr) const -> bool { return !(*this == itr); }

  auto GetBPM() -> BufferPoolManager * { return bpm_; }
  auto GetReadGuard() -> ReadPageGuard { return std::move(read_guard_); }
  auto GetPos() -> int { return pos_; }

 private:
  // add your own private member variables here
  BufferPoolManager *bpm_;
  ReadPageGuard read_guard_;
  int pos_;
};

}  // namespace bustub
