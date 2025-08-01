//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager.h"

namespace bustub {

/**
 * @brief The constructor for a `FrameHeader` that initializes all fields to default values.
 *
 * See the documentation for `FrameHeader` in "buffer/buffer_pool_manager.h" for more information.
 *
 * @param frame_id The frame ID / index of the frame we are creating a header for.
 */
FrameHeader::FrameHeader(frame_id_t frame_id)
    : frame_id_(frame_id), data_(BUSTUB_PAGE_SIZE, 0), page_id_(INVALID_PAGE_ID) {
  Reset();
}

/**
 * @brief Get a raw const pointer to the frame's data.
 *
 * @return const char* A pointer to immutable data that the frame stores.
 */
auto FrameHeader::GetData() const -> const char * { return data_.data(); }

/**
 * @brief Get a raw mutable pointer to the frame's data.
 *
 * @return char* A pointer to mutable data that the frame stores.
 */
auto FrameHeader::GetDataMut() -> char * { return data_.data(); }

/**
 * @brief Resets a `FrameHeader`'s member fields.
 */
void FrameHeader::Reset() {
  std::fill(data_.begin(), data_.end(), 0);
  pin_count_.store(0);
  is_dirty_ = false;
  page_id_ = INVALID_PAGE_ID;
}

/**
 * @brief Creates a new `BufferPoolManager` instance and initializes all fields.
 *
 * See the documentation for `BufferPoolManager` in "buffer/buffer_pool_manager.h" for more information.
 *
 * ### Implementation
 *
 * We have implemented the constructor for you in a way that makes sense with our reference solution. You are free to
 * change anything you would like here if it doesn't fit with you implementation.
 *
 * Be warned, though! If you stray too far away from our guidance, it will be much harder for us to help you. Our
 * recommendation would be to first implement the buffer pool manager using the stepping stones we have provided.
 *
 * Once you have a fully working solution (all Gradescope test cases pass), then you can try more interesting things!
 *
 * @param num_frames The size of the buffer pool.
 * @param disk_manager The disk manager.
 * @param k_dist The backward k-distance for the LRU-K replacer.
 * @param log_manager The log manager. Please ignore this for P1.
 */
BufferPoolManager::BufferPoolManager(size_t num_frames, DiskManager *disk_manager, size_t k_dist,
                                     LogManager *log_manager)
    : num_frames_(num_frames),
      next_page_id_(0),
      bpm_latch_(std::make_shared<std::mutex>()),
      replacer_(std::make_shared<LRUKReplacer>(num_frames, k_dist)),
      disk_scheduler_(std::make_shared<DiskScheduler>(disk_manager)),
      log_manager_(log_manager) {
  // Not strictly necessary...
  std::scoped_lock latch(*bpm_latch_);

  // Initialize the monotonically increasing counter at 0.
  next_page_id_.store(0);

  // Allocate all of the in-memory frames up front.
  frames_.reserve(num_frames_);

  // The page table should have exactly `num_frames_` slots, corresponding to exactly `num_frames_` frames.
  page_table_.reserve(num_frames_);

  // Initialize all of the frame headers, and fill the free frame list with all possible frame IDs (since all frames are
  // initially free).
  for (size_t i = 0; i < num_frames_; i++) {
    frames_.push_back(std::make_shared<FrameHeader>(i));
    free_frames_.push_back(static_cast<int>(i));
  }
}

/**
 * @brief Destroys the `BufferPoolManager`, freeing up all memory that the buffer pool was using.
 */
BufferPoolManager::~BufferPoolManager() = default;

/**
 * @brief Returns the number of frames that this buffer pool manages.
 */
auto BufferPoolManager::Size() const -> size_t { return num_frames_; }

/**
 * @brief Allocates a new page on disk.
 *
 * ### Implementation
 *
 * You will maintain a thread-safe, monotonically increasing counter in the form of a `std::atomic<page_id_t>`.
 * See the documentation on [atomics](https://en.cppreference.com/w/cpp/atomic/atomic) for more information.
 *
 * TODO(P1): Add implementation.
 *
 * @return The page ID of the newly allocated page.
 */
auto BufferPoolManager::NewPage() -> page_id_t {
  std::scoped_lock latch(*bpm_latch_);
  frame_id_t frame_id;

  if (!free_frames_.empty()) {
    frame_id = free_frames_.front();
    free_frames_.pop_front();
  } else {
    const auto opt = replacer_->Evict();
    if (opt == std::nullopt) {
      return INVALID_PAGE_ID;
    }

    frame_id = opt.value();
    const auto &frame = frames_[frame_id];
    const page_id_t old_page_id = frame->page_id_;

    // 如果旧页是脏的，则写回
    // 直接将页面数据写回磁盘
    auto promise = disk_scheduler_->CreatePromise();
    auto future = promise.get_future();
    disk_scheduler_->Schedule({/*is_write=*/true, frame->GetDataMut(), old_page_id, std::move(promise)});
    if (!future.get()) {
      return INVALID_PAGE_ID;
    }

    page_table_.erase(old_page_id);
    frame->Reset();
  }

  // 只有在成功获取 frame 后才分配 page_id
  const page_id_t page_id = next_page_id_.fetch_add(1);

  // 更新新页元信息
  const auto &new_frame = frames_[frame_id];
  new_frame->page_id_ = page_id;
  // 刚创建的新页面当前没有被任何线程持有，因此 pin 计数应当从 0 开始。
  new_frame->pin_count_.store(0);

  page_table_[page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  // 新建页面在被首次访问前不应该被驱逐，因此仍然标记为不可驱逐。
  replacer_->SetEvictable(frame_id, false);

  // 将新页面写入磁盘以确保持久化
  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();
  disk_scheduler_->Schedule({/*is_write=*/true, new_frame->GetDataMut(), page_id, std::move(promise)});

  if (!future.get()) {
    // 磁盘写入失败，需要清理
    page_table_.erase(page_id);
    new_frame->Reset();
    free_frames_.push_back(frame_id);
    return INVALID_PAGE_ID;
  }

  return page_id;
}

/**
 * @brief Removes a page from the database, both on disk and in memory.
 *
 * If the page is pinned in the buffer pool, this function does nothing and returns `false`. Otherwise, this function
 * removes the page from both disk and memory (if it is still in the buffer pool), returning `true`.
 *
 * ### Implementation
 *
 * Think about all of the places a page or a page's metadata could be, and use that to guide you on implementing this
 * function. You will probably want to implement this function _after_ you have implemented `CheckedReadPage` and
 * `CheckedWritePage`.
 *
 * You should call `DeallocatePage` in the disk scheduler to make the space available for new pages.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The page ID of the page we want to delete.
 * @return `false` if the page exists but could not be deleted, `true` if the page didn't exist or deletion succeeded.
 */
auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  std::scoped_lock latch(*bpm_latch_);

  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return true;  // 页面不存在，认为删除成功
  }

  frame_id_t frame_id = it->second;
  auto &frame = frames_[frame_id];

  if (frame->pin_count_.load() > 0) {
    return false;  // 页面被 pinned，无法删除
  }

  // 从磁盘删除
  disk_scheduler_->DeallocatePage(page_id);

  // 从内存删除
  page_table_.erase(page_id);
  frame->Reset();
  free_frames_.push_back(frame_id);

  return true;
}

/**
 * @brief Acquires an optional write-locked guard over a page of data. The user can specify an `AccessType` if needed.
 *
 * If it is not possible to bring the page of data into memory, this function will return a `std::nullopt`.
 *
 * Page data can _only_ be accessed via page guards. Users of this `BufferPoolManager` are expected to acquire either a
 * `ReadPageGuard` or a `WritePageGuard` depending on the mode in which they would like to access the data, which
 * ensures that any access of data is thread-safe.
 *
 * There can only be 1 `WritePageGuard` reading/writing a page at a time. This allows data access to be both immutable
 * and mutable, meaning the thread that owns the `WritePageGuard` is allowed to manipulate the page's data however they
 * want. If a user wants to have multiple threads reading the page at the same time, they must acquire a `ReadPageGuard`
 * with `CheckedReadPage` instead.
 *
 * ### Implementation
 *
 * There are 3 main cases that you will have to implement. The first two are relatively simple: one is when there is
 * plenty of available memory, and the other is when we don't actually need to perform any additional I/O. Think about
 * what exactly these two cases entail.
 *
 * The third case is the trickiest, and it is when we do not have any _easily_ available memory at our disposal. The
 * buffer pool is tasked with finding memory that it can use to bring in a page of memory, using the replacement
 * algorithm you implemented previously to find candidate frames for eviction.
 *
 * Once the buffer pool has identified a frame for eviction, several I/O operations may be necessary to bring in the
 * page of data we want into the frame.
 *
 * There is likely going to be a lot of shared code with `CheckedReadPage`, so you may find creating helper functions
 * useful.
 *
 * These two functions are the crux of this project, so we won't give you more hints than this. Good luck!
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The ID of the page we want to write to.
 * @param access_type The type of page access.
 * @return std::optional<WritePageGuard> An optional latch guard where if there are no more free frames (out of memory)
 * returns `std::nullopt`, otherwise returns a `WritePageGuard` ensuring exclusive and mutable access to a page's data.
 */
auto BufferPoolManager::CheckedWritePage(page_id_t page_id, AccessType access_type) -> std::optional<WritePageGuard> {
  // 若请求的 page_id 非法，直接返回失败。
  if (page_id == INVALID_PAGE_ID) {
    return std::nullopt;
  }
  std::unique_lock latch(*bpm_latch_);

  if (const auto it = page_table_.find(page_id); it != page_table_.end()) {
    const auto &frame = frames_[it->second];
    frame->pin_count_.fetch_add(1);
    replacer_->RecordAccess(it->second);
    replacer_->SetEvictable(it->second, false);

    latch.unlock();
    frame->rwlatch_.lock();

    return WritePageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
  }

  frame_id_t frame_id;
  if (!free_frames_.empty()) {
    frame_id = free_frames_.front();
    free_frames_.pop_front();
  } else {
    const auto opt = replacer_->Evict();
    if (opt == std::nullopt) {
      return std::nullopt;
    }

    frame_id = opt.value();
    const auto &frame = frames_[frame_id];
    const page_id_t old_page_id = frame->page_id_;

    // 如果旧页是脏的，则写回
    if (frame->is_dirty_) {
      // 直接将页面数据写回磁盘
      auto promise = disk_scheduler_->CreatePromise();
      auto future = promise.get_future();
      disk_scheduler_->Schedule({/*is_write=*/true, frame->GetDataMut(), old_page_id, std::move(promise)});
      if (!future.get()) {
        return std::nullopt;
      }
    }

    page_table_.erase(old_page_id);
    frame->Reset();
  }

  const auto &frame = frames_[frame_id];
  frame->page_id_ = page_id;
  frame->pin_count_.fetch_add(1);

  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();
  disk_scheduler_->Schedule({false, frame->GetDataMut(), page_id, std::move(promise)});
  if (!future.get()) {
    return std::nullopt;
  }

  page_table_[page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  latch.unlock();
  frame->rwlatch_.lock();

  return WritePageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
}

/**
 * @brief Acquires an optional read-locked guard over a page of data. The user can specify an `AccessType` if needed.
 *
 * If it is not possible to bring the page of data into memory, this function will return a `std::nullopt`.
 *
 * Page data can _only_ be accessed via page guards. Users of this `BufferPoolManager` are expected to acquire either a
 * `ReadPageGuard` or a `WritePageGuard` depending on the mode in which they would like to access the data, which
 * ensures that any access of data is thread-safe.
 *
 * There can be any number of `ReadPageGuard`s reading the same page of data at a time across different threads.
 * However, all data access must be immutable. If a user wants to mutate the page's data, they must acquire a
 * `WritePageGuard` with `CheckedWritePage` instead.
 *
 * ### Implementation
 *
 * See the implementation details of `CheckedWritePage`.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return std::optional<ReadPageGuard> An optional latch guard where if there are no more free frames (out of memory)
 * returns `std::nullopt`, otherwise returns a `ReadPageGuard` ensuring shared and read-only access to a page's data.
 */
auto BufferPoolManager::CheckedReadPage(page_id_t page_id, AccessType access_type) -> std::optional<ReadPageGuard> {
  // 若请求的 page_id 非法，直接返回失败。
  if (page_id == INVALID_PAGE_ID) {
    return std::nullopt;
  }
  std::unique_lock latch(*bpm_latch_);
  if (const auto it = page_table_.find(page_id); it != page_table_.end()) {
    const auto &frame = frames_[it->second];
    frame->pin_count_.fetch_add(1);
    replacer_->RecordAccess(it->second);
    replacer_->SetEvictable(it->second, false);

    latch.unlock();
    frame->rwlatch_.lock_shared();

    return ReadPageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
  }

  frame_id_t frame_id;
  if (!free_frames_.empty()) {
    frame_id = free_frames_.front();
    free_frames_.pop_front();
  } else {
    const auto opt = replacer_->Evict();
    if (opt == std::nullopt) {
      return std::nullopt;
    }

    frame_id = opt.value();
    const auto &frame = frames_[frame_id];
    const page_id_t old_page_id = frame->page_id_;

    // 如果旧页是脏的，则写回
    if (frame->is_dirty_) {
      // 直接将页面数据写回磁盘
      auto promise = disk_scheduler_->CreatePromise();
      auto future = promise.get_future();
      disk_scheduler_->Schedule({/*is_write=*/true, frame->GetDataMut(), old_page_id, std::move(promise)});
      if (!future.get()) {
        return std::nullopt;
      }
    }

    page_table_.erase(old_page_id);
    frame->Reset();
  }

  const auto &frame = frames_[frame_id];
  frame->page_id_ = page_id;
  frame->pin_count_.fetch_add(1);

  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();
  disk_scheduler_->Schedule({false, frame->GetDataMut(), page_id, std::move(promise)});
  if (!future.get()) {
    return std::nullopt;
  }

  page_table_[page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  latch.unlock();
  frame->rwlatch_.lock_shared();

  return ReadPageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
}

/**
 * @brief A wrapper around `CheckedWritePage` that unwraps the inner value if it exists.
 *
 * If `CheckedWritePage` returns a `std::nullopt`, **this function aborts the entire process.**
 *
 * This function should **only** be used for testing and ergonomic's sake. If it is at all possible that the buffer pool
 * manager might run out of memory, then use `CheckedPageWrite` to allow you to handle that case.
 *
 * See the documentation for `CheckedPageWrite` for more information about implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return WritePageGuard A page guard ensuring exclusive and mutable access to a page's data.
 */
auto BufferPoolManager::WritePage(page_id_t page_id, AccessType access_type) -> WritePageGuard {
  auto guard_opt = CheckedWritePage(page_id, access_type);

  if (!guard_opt.has_value()) {
    fmt::println(stderr, "\n`CheckedWritePage` failed to bring in page {}\n", page_id);
    std::abort();
  }

  return std::move(guard_opt).value();
}

/**
 * @brief A wrapper around `CheckedReadPage` that unwraps the inner value if it exists.
 *
 * If `CheckedReadPage` returns a `std::nullopt`, **this function aborts the entire process.**
 *
 * This function should **only** be used for testing and ergonomic's sake. If it is at all possible that the buffer pool
 * manager might run out of memory, then use `CheckedPageWrite` to allow you to handle that case.
 *
 * See the documentation for `CheckedPageRead` for more information about implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return ReadPageGuard A page guard ensuring shared and read-only access to a page's data.
 */
auto BufferPoolManager::ReadPage(page_id_t page_id, AccessType access_type) -> ReadPageGuard {
  auto guard_opt = CheckedReadPage(page_id, access_type);

  if (!guard_opt.has_value()) {
    fmt::println(stderr, "\n`CheckedReadPage` failed to bring in page {}\n", page_id);
    std::abort();
  }

  return std::move(guard_opt).value();
}

/**
 * @brief Flushes a page's data out to disk unsafely.
 *
 * This function will write out a page's data to disk if it has been modified. If the given page is not in memory, this
 * function will return `false`.
 *
 * You should not take a lock on the page in this function.
 * This means that you should carefully consider when to toggle the `is_dirty_` bit.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage` and
 * `CheckedWritePage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page to be flushed.
 * @return `false` if the page could not be found in the page table, otherwise `true`.
 */
auto BufferPoolManager::FlushPageUnsafe(page_id_t page_id) -> bool {
  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  const auto &frame = frames_[it->second];
  if (frame->is_dirty_) {
    auto promise = disk_scheduler_->CreatePromise();
    auto future = promise.get_future();

    // 使用 const_cast 转换类型，因为磁盘写入不会修改数据
    disk_scheduler_->Schedule({true, const_cast<char *>(frame->GetData()), page_id, std::move(promise)});

    if (future.get()) {
      // 写入成功，清除脏标记
      frame->is_dirty_ = false;
    }
  }
  return true;
}

/**
 * @brief Flushes a page's data out to disk safely.
 *
 * This function will write out a page's data to disk if it has been modified. If the given page is not in memory, this
 * function will return `false`.
 *
 * You should take a lock on the page in this function to ensure that a consistent state is flushed to disk.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `Flush` in the page guards, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page to be flushed.
 * @return `false` if the page could not be found in the page table, otherwise `true`.
 */
auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  std::scoped_lock latch(*bpm_latch_);
  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  const auto &frame = frames_[it->second];
  if (frame->is_dirty_) {
    auto promise = disk_scheduler_->CreatePromise();
    auto future = promise.get_future();

    // 使用 const_cast 转换类型，因为磁盘写入不会修改数据
    disk_scheduler_->Schedule({true, const_cast<char *>(frame->GetData()), page_id, std::move(promise)});

    if (future.get()) {
      // 写入成功，清除脏标记
      frame->is_dirty_ = false;
    }
  }
  return true;
}

/**
 * @brief Flushes all page data that is in memory to disk unsafely.
 *
 * You should not take locks on the pages in this function.
 * This means that you should carefully consider when to toggle the `is_dirty_` bit.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `FlushPage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 */
void BufferPoolManager::FlushAllPagesUnsafe() {
  for (auto &[k, v] : page_table_) {
    FlushPageUnsafe(k);
  }
}

/**
 * @brief Flushes all page data that is in memory to disk safely.
 *
 * You should take locks on the pages in this function to ensure that a consistent state is flushed to disk.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `FlushPage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 */
void BufferPoolManager::FlushAllPages() {
  std::unique_lock latch(*bpm_latch_);

  // 先收集所有页面 ID，然后释放锁再 flush
  std::vector<page_id_t> page_ids;
  page_ids.reserve(page_table_.size());
  for (const auto &[page_id, frame_id] : page_table_) {
    page_ids.push_back(page_id);
  }

  latch.unlock();  // 释放锁

  // 现在可以安全地 flush 每个页面
  for (page_id_t page_id : page_ids) {
    FlushPage(page_id);  // 使用 FlushPage 而不是 FlushPageUnsafe
  }
}

/**
 * @brief Retrieves the pin count of a page. If the page does not exist in memory, return `std::nullopt`.
 *
 * This function is thread safe. Callers may invoke this function in a multi-threaded environment where multiple threads
 * access the same page.
 *
 * This function is intended for testing purposes. If this function is implemented incorrectly, it will definitely cause
 * problems with the test suite and autograder.
 *
 * # Implementation
 *
 * We will use this function to test if your buffer pool manager is managing pin counts correctly. Since the
 * `pin_count_` field in `FrameHeader` is an atomic type, you do not need to take the latch on the frame that holds the
 * page we want to look at. Instead, you can simply use an atomic `load` to safely load the value stored. You will still
 * need to take the buffer pool latch, however.
 *
 * Again, if you are unfamiliar with atomic types, see the official C++ docs
 * [here](https://en.cppreference.com/w/cpp/atomic/atomic).
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page we want to get the pin count of.
 * @return std::optional<size_t> The pin count if the page exists, otherwise `std::nullopt`.
 */
auto BufferPoolManager::GetPinCount(page_id_t page_id) -> std::optional<size_t> {
  std::scoped_lock latch(*bpm_latch_);
  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return std::nullopt;
  }

  return frames_[it->second]->pin_count_.load();
}

}  // namespace bustub
