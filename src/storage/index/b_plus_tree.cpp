//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree.cpp
//
// Identification: src/storage/index/b_plus_tree.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/index/b_plus_tree.h"
#include "common/config.h"
#include "storage/index/b_plus_tree_debug.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

#include <algorithm>

/*
get a page using buffer pool manager (bpm) when creating a new node
each node contains a whole page

WritePageGuard guard = bpm_->WritePage(header_page_id_);
auto root_page = guard.AsMut<BPlusTreeHeaderPage>();
root_page->root_page_id_ = INVALID_PAGE_ID;

use header_page_id to get w page guard
*/

namespace bustub {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator, int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  WritePageGuard guard = bpm_->WritePage(header_page_id_);
  auto root_page = guard.AsMut<BPlusTreeHeaderPage>();
  root_page->root_page_id_ = INVALID_PAGE_ID;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::InternalLowerBound(const InternalPage *internal, const KeyType &target) -> int {
  int n = internal->GetSize();
  if (n == 0) {
    return 0;  // 空内部结点，直接返回最左指针
  }

  // 若 target 小于第一个有效 key，走最左指针
  if (comparator_(target, internal->KeyAt(1)) < 0) {
    return 0;
  }

  // 二分查找满足 key[i] <= target < key[i+1] 的最大 i
  int left = 1;
  int right = n;  // keys index range [1, n]
  while (left <= right) {
    int mid = left + ((right - left) / 2);
    if (comparator_(target, internal->KeyAt(mid)) < 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  // 循环结束时 right 是最后一个 key <= target 的位置
  return right;
}

/**
 * @brief Helper function to decide whether current b+tree is empty
 * @return Returns true if this B+ tree has no keys and values.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  const auto guard = bpm_->ReadPage(header_page_id_);
  const auto page = guard.As<BPlusTreeHeaderPage>();
  return page->root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/**
 * @brief Return the only value that associated with input key
 *
 * This method is used for point query
 *
 * @param key input key
 * @param[out] result vector that stores the only value that associated with input key, if the value exists
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool {
  if (IsEmpty()) {
    return false;
  }

  Context ctx;
  ctx.read_set_.emplace_back(bpm_->ReadPage(header_page_id_));
  auto &header_guard = ctx.read_set_.back();
  auto header_page = header_guard.As<BPlusTreeHeaderPage>();

  page_id_t root_page_id = header_page->root_page_id_;
  ctx.read_set_.emplace_back(bpm_->ReadPage(root_page_id));

  while (!ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    auto node = ctx.read_set_.back().As<InternalPage>();
    int idx = InternalLowerBound(node, key);
    const page_id_t page_id = node->ValueAt(idx);
    ctx.read_set_.emplace_back(bpm_->ReadPage(page_id));
  }

  auto leaf = ctx.read_set_.back().As<LeafPage>();
  const int size = leaf->GetSize();
  for (int i = 0; i < size; ++i) {
    if (comparator_(leaf->KeyAt(i), key) == 0) {
      result->push_back(leaf->ValueAt(i));
      return true;
    }
  }

  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/**
 * @brief Insert constant key & value pair into b+ tree
 *
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 *
 * @param key the key to insert
 * @param value the value associated with key
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value) -> bool {
  Context ctx;
  ctx.write_set_.emplace_back(bpm_->WritePage(header_page_id_));
  auto &header_guard = ctx.write_set_.back();
  auto header_page = header_guard.AsMut<BPlusTreeHeaderPage>();

  bool tree_empty = (header_page->root_page_id_ == INVALID_PAGE_ID);

  if (tree_empty) {
    page_id_t page_id = bpm_->NewPage();
    ctx.write_set_.emplace_back(bpm_->WritePage(page_id));

    // 更新header page中的root_page_id
    header_page->root_page_id_ = page_id;

    auto &guard = ctx.write_set_.back();
    auto leaf = guard.AsMut<LeafPage>();
    leaf->Init(leaf_max_size_);
    leaf->SetKeyAt(0, key);
    leaf->SetValueAt(0, value);
    leaf->SetSize(1);
    leaf->SetNextPageId(INVALID_PAGE_ID);
    return true;
  }

  page_id_t root_page_id = header_page->root_page_id_;

  // 从 root 开始一路写锁下去，压入 write_set_
  ctx.write_set_.emplace_back(bpm_->WritePage(root_page_id));
  while (!ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    auto node = ctx.write_set_.back().As<InternalPage>();
    int idx = InternalLowerBound(node, key);
    page_id_t child_page_id = node->ValueAt(idx);
    ctx.write_set_.emplace_back(bpm_->WritePage(child_page_id));
  }

  auto &leaf_guard = ctx.write_set_.back();
  auto leaf = leaf_guard.AsMut<LeafPage>();
  page_id_t old_leaf_page_id = leaf_guard.GetPageId();
  int size = leaf->GetSize();

  // 检查重复
  for (int i = 0; i < size; ++i) {
    if (comparator_(leaf->KeyAt(i), key) == 0) {
      return false;
    }
  }

  // 没满，直接插入
  if (size < leaf->GetMaxSize()) {
    int i = size;
    while (i > 0 && comparator_(leaf->KeyAt(i - 1), key) > 0) {
      leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->GetValueAt(i - 1));
      i--;
    }
    leaf->SetKeyAt(i, key);
    leaf->SetValueAt(i, value);
    leaf->IncreaseSize();
    return true;
  }

  // 满了，需要 split
  auto split_leaf = [&](LeafPage *old_leaf, InternalPage *parent) {
    page_id_t new_leaf_page_id = bpm_->NewPage();
    ctx.write_set_.emplace_back(bpm_->WritePage(new_leaf_page_id));
    auto &new_leaf_guard = ctx.write_set_.back();
    auto new_leaf = new_leaf_guard.AsMut<LeafPage>();
    new_leaf->Init(leaf_max_size_);

    int move_start = (leaf_max_size_ + 1) / 2;
    int j = 0;
    for (int i = move_start; i < leaf_max_size_; ++i) {
      new_leaf->SetKeyAt(j, old_leaf->KeyAt(i));
      new_leaf->SetValueAt(j, old_leaf->GetValueAt(i));
      ++j;
    }
    new_leaf->SetSize(j);
    old_leaf->SetSize(move_start);

    new_leaf->SetNextPageId(old_leaf->GetNextPageId());
    old_leaf->SetNextPageId(new_leaf_page_id);

    KeyType up_key = new_leaf->KeyAt(0);
    int insert_idx = InternalLowerBound(parent, up_key);
    int n_parent = parent->GetSize();
    // Shift keys to make space for the new key.
    for (int i = n_parent; i >= insert_idx + 1; --i) {
      parent->SetKeyAt(i + 1, parent->KeyAt(i));
    }
    // Shift pointers to make space for the new pointer.
    for (int i = n_parent; i >= insert_idx + 1; --i) {
      parent->SetValueAt(i + 1, parent->ValueAt(i));
    }
    parent->SetKeyAt(insert_idx + 1, up_key);
    parent->SetValueAt(insert_idx + 1, new_leaf_page_id);
    parent->IncreaseSize();
  };

  auto split_internal = [&](InternalPage *old_internal, InternalPage *parent) {
    page_id_t new_internal_page_id = bpm_->NewPage();
    ctx.write_set_.emplace_back(bpm_->WritePage(new_internal_page_id));
    auto &new_internal_guard = ctx.write_set_.back();
    auto new_internal = new_internal_guard.AsMut<InternalPage>();
    new_internal->Init(internal_max_size_);

    int total_keys = old_internal->GetSize();           // 当前内部结点键数量 (>= 1)
    int left_key_cnt = total_keys / 2;                  // 左结点保留键数量 (= floor)
    int right_key_cnt = total_keys - left_key_cnt - 1;  // 右结点键数量 (>= floor)

    // middle key (index = left_key_cnt + 1) 上升到父节点
    KeyType up_key = old_internal->KeyAt(left_key_cnt + 1);

    // ------------------- 搬运到 new_internal -------------------
    // 最左指针: old_ptr[left_key_cnt + 1]
    new_internal->SetValueAt(0, old_internal->ValueAt(left_key_cnt + 1));

    int new_idx = 0;  // new_internal 键的写入计数
    // 复制右侧键及其右指针
    for (int i = left_key_cnt + 2; i <= total_keys; ++i) {
      // key i 对应 new_internal 的 key (new_idx+1)
      new_internal->SetKeyAt(new_idx + 1, old_internal->KeyAt(i));
      // 右指针 = old_ptr[i]
      new_internal->SetValueAt(new_idx + 1, old_internal->ValueAt(i));
      ++new_idx;
    }
    // 到此 new_internal 共 right_key_cnt 个键，指针数量 = right_key_cnt + 1 (0..right_key_cnt)

    // 更新 size
    new_internal->SetSize(right_key_cnt);
    old_internal->SetSize(left_key_cnt);

    int insert_idx = InternalLowerBound(parent, up_key);
    int n_parent = parent->GetSize();
    // Shift keys to make space for the new key.
    for (int i = n_parent; i >= insert_idx + 1; --i) {
      parent->SetKeyAt(i + 1, parent->KeyAt(i));
    }
    // Shift pointers to make space for the new pointer.
    for (int i = n_parent; i >= insert_idx + 1; --i) {
      parent->SetValueAt(i + 1, parent->ValueAt(i));
    }
    parent->SetKeyAt(insert_idx + 1, up_key);
    parent->SetValueAt(insert_idx + 1, new_internal_page_id);
    parent->IncreaseSize();
  };

  // 从下往上找最先没满的 internal，或者创建新的 root
  int n = static_cast<int>(ctx.write_set_.size());
  int split_idx = n - 2;
  while (split_idx > 0 && ctx.write_set_[split_idx].As<InternalPage>()->GetSize() == internal_max_size_) {
    split_idx--;
  }

  if (split_idx == 0) {
    // 新建 root（插入到 header_guard 之后，保持加锁顺序）
    page_id_t new_root_page_id = bpm_->NewPage();
    ctx.write_set_.emplace(ctx.write_set_.begin() + 1, bpm_->WritePage(new_root_page_id));

    auto &new_root_guard = ctx.write_set_[1];
    header_page->root_page_id_ = new_root_page_id;

    new_root_guard.AsMut<InternalPage>()->Init(internal_max_size_);
    // 新根在插入第一个分裂键之前没有有效的键，因此 size 仍保持为 0。
    new_root_guard.AsMut<InternalPage>()->SetValueAt(0, root_page_id);

    split_idx = 1;  // new_root 位于 header_guard 之后
  }

  // 先分裂叶子
  size_t leaf_idx = ctx.write_set_.size() - 1;
  size_t parent_idx = leaf_idx - 1;
  split_leaf(ctx.write_set_[leaf_idx].AsMut<LeafPage>(), ctx.write_set_[parent_idx].AsMut<InternalPage>());

  // 在调用 split_leaf 之后，新叶子会被压入 write_set_ 的末尾，此时记录其指针以及页面 ID，
  // 以防接下来内部节点的分裂导致 write_set_ 再次变化。
  auto new_leaf = ctx.write_set_.back().AsMut<LeafPage>();
  page_id_t new_leaf_page_id = ctx.write_set_.back().GetPageId();

  // 叶子分裂后，可能使其父节点溢出，再自下而上分裂 internal
  size_t cur = parent_idx;
  while (cur > static_cast<size_t>(split_idx)) {
    if (ctx.write_set_[cur].As<InternalPage>()->GetSize() <= internal_max_size_) {
      break;  // 不再溢出
    }
    split_internal(ctx.write_set_[cur].AsMut<InternalPage>(), ctx.write_set_[cur - 1].AsMut<InternalPage>());
    --cur;  // split_internal 在末尾 push 了 new_internal，当前 old_internal 位置不变，下次检查其父
  }

  // 根据 key 与 new_leaf 最小键的大小关系，决定插入到旧叶子还是新叶子
  LeafPage *target_leaf = nullptr;
  if (comparator_(key, new_leaf->KeyAt(0)) < 0) {
    target_leaf = leaf;  // 插入到旧叶子
  } else {
    target_leaf = new_leaf;  // 插入到新叶子
  }

  int t_size = target_leaf->GetSize();
  int i = t_size;
  while (i > 0 && comparator_(target_leaf->KeyAt(i - 1), key) > 0) {
    target_leaf->SetKeyAt(i, target_leaf->KeyAt(i - 1));
    target_leaf->SetValueAt(i, target_leaf->GetValueAt(i - 1));
    --i;
  }
  target_leaf->SetKeyAt(i, key);
  target_leaf->SetValueAt(i, value);
  target_leaf->IncreaseSize();

  // 如果插入位置在叶子最前端，需要更新父节点中的分隔键
  if (i == 0) {
    // 需要查找包含 target_leaf 的父节点（内部节点可能已经分裂）。
    page_id_t target_pid = (target_leaf == new_leaf) ? new_leaf_page_id : old_leaf_page_id;
    KeyType new_min_key = target_leaf->KeyAt(0);

    for (auto it = ctx.write_set_.rbegin(); it != ctx.write_set_.rend(); ++it) {
      if (it->As<BPlusTreePage>()->IsLeafPage()) {
        continue;  // 只关注内部节点
      }
      auto internal = it->AsMut<InternalPage>();
      int child_idx = internal->ValueIndex(target_pid);
      if (child_idx != -1 && child_idx > 0) {  // index 0 的 key 始终无效
        internal->SetKeyAt(child_idx, new_min_key);
      }
    }
  }

  return true;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/

/*****************************************************************************
 * REMOVE 入口
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key) {
  if (IsEmpty()) {
    return;
  }

  Context ctx;
  ctx.write_set_.emplace_back(bpm_->WritePage(header_page_id_));
  auto header_page = ctx.write_set_.back().AsMut<BPlusTreeHeaderPage>();
  page_id_t root_pid = header_page->root_page_id_;

  /* 自上而下加写锁直到叶子 */
  ctx.write_set_.emplace_back(bpm_->WritePage(root_pid));
  while (!ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    auto node = ctx.write_set_.back().As<InternalPage>();
    int idx = InternalLowerBound(node, key);
    ctx.write_set_.emplace_back(bpm_->WritePage(node->ValueAt(idx)));
  }

  RemoveKeyAndRebalance(key, ctx);
}

/*****************************************************************************
 * REMOVE helpers
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveKeyAndRebalance(const KeyType &key, Context &ctx) {
  /* 1. 从叶子节点删除目标键 */
  auto &leaf_guard = ctx.write_set_.back();
  auto leaf = leaf_guard.template AsMut<LeafPage>();

  int del_pos = -1;
  for (int i = 0; i < leaf->GetSize(); ++i) {
    if (comparator_(leaf->KeyAt(i), key) == 0) {
      del_pos = i;
      break;
    }
  }
  if (del_pos == -1) {
    return;  // 未找到
  }

  for (int i = del_pos + 1; i < leaf->GetSize(); ++i) {
    leaf->SetKeyAt(i - 1, leaf->KeyAt(i));
    leaf->SetValueAt(i - 1, leaf->ValueAt(i));
  }
  leaf->DecreaseSize();

  /* 2. 若目前整棵树只有一个叶子节点 */
  if (ctx.write_set_.size() == 2) {  // header + leaf
    if (leaf->GetSize() == 0) {
      ctx.write_set_.front().AsMut<BPlusTreeHeaderPage>()->root_page_id_ = INVALID_PAGE_ID;
    }
    return;
  }

  int leaf_min_size = (leaf_max_size_ + 1) / 2;

  auto &parent_guard = ctx.write_set_[ctx.write_set_.size() - 2];
  auto parent = parent_guard.template AsMut<InternalPage>();
  int idx_in_parent = parent->ValueIndex(leaf_guard.GetPageId());

  /* 3. 如果删除后叶子节点仍然满足最小大小 */
  if (leaf->GetSize() >= leaf_min_size) {
    if (del_pos == 0 && idx_in_parent > 0) {
      parent->SetKeyAt(idx_in_parent, leaf->KeyAt(0));
    }
    return;
  }

  /* 4. 尝试向左右兄弟借一个键 */
  auto borrow_from_left_leaf = [&]() -> bool {
    if (idx_in_parent == 0) {
      return false;
    }
    ctx.write_set_.emplace_back(bpm_->WritePage(parent->ValueAt(idx_in_parent - 1)));
    auto &left_guard = ctx.write_set_.back();
    auto left = left_guard.template AsMut<LeafPage>();
    if (left->GetSize() <= leaf_min_size) {
      left_guard.Drop();
      ctx.write_set_.pop_back();
      return false;
    }

    // 将左兄弟的最后一个键移到当前叶子的最前端
    for (int i = leaf->GetSize(); i > 0; --i) {
      leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
    }
    leaf->SetKeyAt(0, left->KeyAt(left->GetSize() - 1));
    leaf->SetValueAt(0, left->ValueAt(left->GetSize() - 1));
    leaf->IncreaseSize();
    left->DecreaseSize();

    parent->SetKeyAt(idx_in_parent, leaf->KeyAt(0));
    return true;
  };

  auto borrow_from_right_leaf = [&]() -> bool {
    if (leaf->GetNextPageId() == INVALID_PAGE_ID) {
      return false;
    }
    page_id_t right_pid = leaf->GetNextPageId();
    ctx.write_set_.emplace_back(bpm_->WritePage(right_pid));
    auto &right_guard = ctx.write_set_.back();
    auto right = right_guard.template AsMut<LeafPage>();
    if (right->GetSize() <= leaf_min_size) {
      right_guard.Drop();
      ctx.write_set_.pop_back();
      return false;
    }

    // 将右兄弟的第一个键添加到当前叶子的末尾
    leaf->SetKeyAt(leaf->GetSize(), right->KeyAt(0));
    leaf->SetValueAt(leaf->GetSize(), right->ValueAt(0));
    leaf->IncreaseSize();

    for (int i = 1; i < right->GetSize(); ++i) {
      right->SetKeyAt(i - 1, right->KeyAt(i));
      right->SetValueAt(i - 1, right->ValueAt(i));
    }
    right->DecreaseSize();

    parent->SetKeyAt(idx_in_parent + 1, right->KeyAt(0));
    return true;
  };

  if (borrow_from_left_leaf() || borrow_from_right_leaf()) {
    return;  // 借成功
  }

  /* 5. 无法借，则与兄弟合并 */
  // 根据父节点判断是否存在右兄弟
  bool merge_with_right = (idx_in_parent < parent->GetSize());

  LeafPage *dst_leaf;  // 合并结果保存在 dst_leaf 中
  LeafPage *src_leaf;  // 被合并并随后删除的叶子
  page_id_t src_pid;
  WritePageGuard *src_guard = nullptr;

  if (merge_with_right) {
    // 将右兄弟合并到当前叶子
    src_pid = parent->ValueAt(idx_in_parent + 1);
    dst_leaf = leaf;
    ctx.write_set_.emplace_back(bpm_->WritePage(src_pid));
    auto &src_guard_ref = ctx.write_set_.back();
    src_guard = &src_guard_ref;
    src_leaf = src_guard_ref.template AsMut<LeafPage>();
  } else {
    // 将当前叶子合并到左兄弟
    page_id_t left_pid = parent->ValueAt(idx_in_parent - 1);
    ctx.write_set_.emplace_back(bpm_->WritePage(left_pid));
    auto &left_guard2 = ctx.write_set_.back();
    dst_leaf = left_guard2.template AsMut<LeafPage>();
    src_pid = leaf_guard.GetPageId();
    src_leaf = leaf;
  }

  int base = dst_leaf->GetSize();
  for (int i = 0; i < src_leaf->GetSize(); ++i) {
    dst_leaf->SetKeyAt(base + i, src_leaf->KeyAt(i));
    dst_leaf->SetValueAt(base + i, src_leaf->ValueAt(i));
  }
  dst_leaf->SetSize(base + src_leaf->GetSize());
  dst_leaf->SetNextPageId(src_leaf->GetNextPageId());

  // 释放写锁后再删除页面，避免死锁
  if (merge_with_right && src_guard != nullptr) {
    src_guard->Drop();
  } else {
    // src_leaf 即 leaf_guard 对应页面，仅释放写锁，不从 write_set_ 删除，避免索引错位
    leaf_guard.Drop();
  }
  bpm_->DeletePage(src_pid);

  /* 从父节点删除对应的键和指针 */
  int remove_idx = merge_with_right ? idx_in_parent + 1 : idx_in_parent;
  int psize = parent->GetSize();
  for (int i = remove_idx; i < psize; ++i) {
    parent->SetKeyAt(i, parent->KeyAt(i + 1));
    parent->SetValueAt(i, parent->ValueAt(i + 1));
  }
  parent->DecreaseSize();
  parent->SetValueAt(parent->GetSize() + 1, INVALID_PAGE_ID);

  /* 6. 自下而上处理内部节点可能出现的欠缺 */
  int cur_idx = static_cast<int>(ctx.write_set_.size()) - 2;  // 从父节点开始
  while (cur_idx > 1) {                                       // 根节点在 write_set_ 的下标 1
    auto &cur_guard = ctx.write_set_[cur_idx];
    auto cur_node = cur_guard.template AsMut<InternalPage>();

    int internal_min_size = std::max(1, ((internal_max_size_ + 1) / 2) - 1);
    if (cur_node->GetSize() >= internal_min_size) {
      break;  // 当前节点已满足最小大小
    }

    auto &p_guard = ctx.write_set_[cur_idx - 1];
    auto p_node = p_guard.template AsMut<InternalPage>();
    int idx_in_p = p_node->ValueIndex(cur_guard.GetPageId());

    bool fixed = false;
    /* 6.1 先尝试向左兄弟借 */
    if (idx_in_p > 0) {
      page_id_t left_pid = p_node->ValueAt(idx_in_p - 1);
      ctx.write_set_.emplace_back(bpm_->WritePage(left_pid));
      auto &left_guard = ctx.write_set_.back();
      auto left_node = left_guard.template AsMut<InternalPage>();
      if (left_node->GetSize() > internal_min_size) {
        // 将父分隔键下沉到当前节点，左兄弟最后一个键上升到父节点
        for (int i = cur_node->GetSize(); i >= 0; --i) {
          cur_node->SetValueAt(i + 1, cur_node->ValueAt(i));
        }
        for (int i = cur_node->GetSize(); i >= 1; --i) {
          cur_node->SetKeyAt(i + 1, cur_node->KeyAt(i));
        }
        cur_node->SetValueAt(0, left_node->ValueAt(left_node->GetSize()));
        cur_node->SetKeyAt(1, p_node->KeyAt(idx_in_p));
        cur_node->IncreaseSize();

        p_node->SetKeyAt(idx_in_p, left_node->KeyAt(left_node->GetSize()));
        left_node->DecreaseSize();
        fixed = true;
      } else {
        left_guard.Drop();
        ctx.write_set_.pop_back();
      }
    }

    /* 6.2 再尝试向右兄弟借 */
    if (!fixed && idx_in_p < p_node->GetSize()) {
      page_id_t right_pid = p_node->ValueAt(idx_in_p + 1);
      ctx.write_set_.emplace_back(bpm_->WritePage(right_pid));
      auto &right_guard = ctx.write_set_.back();
      auto right_node = right_guard.template AsMut<InternalPage>();
      if (right_node->GetSize() > internal_min_size) {
        int csize = cur_node->GetSize();
        cur_node->SetKeyAt(csize + 1, p_node->KeyAt(idx_in_p + 1));
        cur_node->SetValueAt(csize + 1, right_node->ValueAt(0));
        cur_node->IncreaseSize();

        for (int i = 0; i < right_node->GetSize(); ++i) {
          right_node->SetValueAt(i, right_node->ValueAt(i + 1));
        }
        for (int i = 1; i <= right_node->GetSize(); ++i) {
          right_node->SetKeyAt(i, right_node->KeyAt(i + 1));
        }
        right_node->DecreaseSize();

        p_node->SetKeyAt(idx_in_p + 1, right_node->KeyAt(1));
        fixed = true;
      } else {
        right_guard.Drop();
        ctx.write_set_.pop_back();
      }
    }

    if (fixed) {
      break;  // 借成功，结束循环
    }

    /* 6.3 无法借，将当前节点与兄弟合并 */
    bool merge_with_right_internal = (idx_in_p < p_node->GetSize());
    InternalPage *dst_internal;
    page_id_t victim_internal_pid;

    if (merge_with_right_internal) {
      victim_internal_pid = p_node->ValueAt(idx_in_p + 1);
      dst_internal = cur_node;
    } else {
      victim_internal_pid = p_node->ValueAt(idx_in_p - 1);
      ctx.write_set_.emplace_back(bpm_->WritePage(victim_internal_pid));
      auto &victim_dst_guard = ctx.write_set_.back();
      dst_internal = victim_dst_guard.template AsMut<InternalPage>();
    }
    if (merge_with_right_internal) {
      ctx.write_set_.emplace_back(bpm_->WritePage(victim_internal_pid));
    }
    auto &victim_internal_guard = ctx.write_set_.back();
    auto victim_internal = victim_internal_guard.template AsMut<InternalPage>();

    int base_keys = dst_internal->GetSize();
    KeyType sep_key = merge_with_right_internal ? p_node->KeyAt(idx_in_p + 1) : p_node->KeyAt(idx_in_p);
    dst_internal->SetKeyAt(base_keys + 1, sep_key);
    dst_internal->SetValueAt(base_keys + 1,
                             merge_with_right_internal ? victim_internal->ValueAt(0) : cur_node->ValueAt(0));

    int offset = 1;
    for (int i = 1; i <= victim_internal->GetSize(); ++i) {
      dst_internal->SetKeyAt(base_keys + 1 + offset, victim_internal->KeyAt(i));
      dst_internal->SetValueAt(base_keys + 1 + offset, victim_internal->ValueAt(i));
      ++offset;
    }
    dst_internal->SetSize(base_keys + 1 + victim_internal->GetSize());

    if (merge_with_right_internal) {
      victim_internal_guard.Drop();
    } else {
      cur_guard.Drop();
    }
    bpm_->DeletePage(merge_with_right_internal ? victim_internal_pid : cur_guard.GetPageId());

    int p_remove_idx = merge_with_right_internal ? idx_in_p + 1 : idx_in_p;
    int psize2 = p_node->GetSize();
    for (int i = p_remove_idx; i < psize2; ++i) {
      p_node->SetKeyAt(i, p_node->KeyAt(i + 1));
      p_node->SetValueAt(i, p_node->ValueAt(i + 1));
    }
    p_node->DecreaseSize();
    p_node->SetValueAt(p_node->GetSize() + 1, INVALID_PAGE_ID);

    --cur_idx;  // 继续向上处理
  }

  /* 7. 如果根内部节点已空，收缩根 */
  if (ctx.write_set_.size() >= 3) {
    auto &root_guard = ctx.write_set_[1];
    auto root_page = root_guard.template AsMut<InternalPage>();
    if (root_page->GetSize() == 0) {
      ctx.write_set_.front().AsMut<BPlusTreeHeaderPage>()->root_page_id_ = root_page->ValueAt(0);
      bpm_->DeletePage(root_guard.GetPageId());
    }
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/**
 * @brief Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 *
 * You may want to implement this while implementing Task #3.
 *
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE {
  if (IsEmpty()) {
    return INDEXITERATOR_TYPE();
  }

  Context ctx;
  ctx.read_set_.emplace_back(bpm_->ReadPage(header_page_id_));
  auto &header_guard = ctx.read_set_.back();
  auto header_page = header_guard.As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = header_page->root_page_id_;
  page_id_t page_id = root_page_id;  // 根节点可能就是叶子

  ctx.read_set_.emplace_back(bpm_->ReadPage(root_page_id));

  while (!ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    auto node = ctx.read_set_.back().As<InternalPage>();
    page_id = node->ValueAt(0);
    ctx.read_set_.emplace_back(bpm_->ReadPage(page_id));
  }

  return INDEXITERATOR_TYPE(bpm_, bpm_->ReadPage(page_id), 0);
}

/**
 * @brief Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  auto itr = Begin();
  while (!itr.IsEnd() && comparator_((*itr).first, key) < 0) {
    ++itr;
  }
  return INDEXITERATOR_TYPE(itr.GetBPM(), itr.GetReadGuard(), itr.GetPos());
}

/**
 * @brief Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE { return INDEXITERATOR_TYPE(); }

/**
 * @return Page id of the root of this tree
 *
 * You may want to implement this while implementing Task #3.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  // return header_page_id_;
  const auto guard = bpm_->ReadPage(header_page_id_);
  const auto page = guard.As<BPlusTreeHeaderPage>();
  return page->root_page_id_;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
