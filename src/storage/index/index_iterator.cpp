//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_iterator.cpp
//
// Identification: src/storage/index/index_iterator.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

/**
 * index_iterator.cpp
 */
#include <cassert>
#include "common/config.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"

#include "storage/index/index_iterator.h"

namespace bustub {

/**
 * @note you can change the destructor/constructor method here
 * set your own input parameters
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator() : bpm_(nullptr), pos_(0) {}

INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator(BufferPoolManager *bpm, ReadPageGuard read_guard, int pos)
    : bpm_(bpm), read_guard_(std::move(read_guard)), pos_(pos) {}

INDEX_TEMPLATE_ARGUMENTS INDEXITERATOR_TYPE::~IndexIterator() = default;  // NOLINT

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::IsEnd() const -> bool { return bpm_ == nullptr; }

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator*() -> std::pair<const KeyType &, const ValueType &> {
  auto leaf = read_guard_.As<LeafPage>();
  return {leaf->KeyAt(pos_), leaf->ValueAt(pos_)};
}

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator++() -> INDEXITERATOR_TYPE & {
  // 如果已经到达末尾，直接返回
  if (bpm_ == nullptr) {
    return *this;
  }

  auto leaf = read_guard_.As<LeafPage>();

  // 1. 尝试在当前页内前进
  if (pos_ + 1 < leaf->GetSize()) {
    ++pos_;
    return *this;
  }

  // 2. 记录下一页页号并释放当前页锁，避免锁顺序问题
  page_id_t next_page_id = leaf->GetNextPageId();
  read_guard_.Drop();

  // 3. 如果没有下一页，则迭代器走到末尾
  if (next_page_id == INVALID_PAGE_ID) {
    bpm_ = nullptr;
    return *this;
  }

  // 4. 读取下一页并重置位置
  read_guard_ = bpm_->ReadPage(next_page_id);
  pos_ = 0;
  return *this;
}

template class IndexIterator<GenericKey<4>, RID, GenericComparator<4>>;

template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>>;

template class IndexIterator<GenericKey<16>, RID, GenericComparator<16>>;

template class IndexIterator<GenericKey<32>, RID, GenericComparator<32>>;

template class IndexIterator<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
