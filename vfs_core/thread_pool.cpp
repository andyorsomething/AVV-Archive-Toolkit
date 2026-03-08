#include "thread_pool.h"

namespace vfs {

// Thread-local memory arena for worker threads
thread_local std::unique_ptr<MemoryArena> tls_arena = nullptr;

MemoryArena::MemoryArena(size_t capacity) { buffer_.resize(capacity); }

MemoryArena::~MemoryArena() = default;

void *MemoryArena::allocate(size_t size, size_t alignment) {
  // align offset
  size_t space = buffer_.size() - offset_;
  void *ptr = buffer_.data() + offset_;
  if (std::align(alignment, size, ptr, space)) {
    offset_ = buffer_.size() - space + size;
    return ptr;
  }
  return nullptr; // out of memory in arena
}

void MemoryArena::reset() { offset_ = 0; }

ThreadPool::ThreadPool(size_t threads) {
  if (threads == 0)
    threads = 1;

  for (size_t i = 0; i < threads; ++i) {
    workers_.emplace_back([this] {
      // Initialize thread-local arena for this worker
      tls_arena = std::make_unique<MemoryArena>();

      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(this->queue_mutex_);
          this->condition_.wait(
              lock, [this] { return this->stop_ || !this->tasks_.empty(); });
          if (this->stop_ && this->tasks_.empty())
            return;
          task = std::move(this->tasks_.front());
          this->tasks_.pop();
        }

        // Reset arena before each task
        tls_arena->reset();
        task();
      }
    });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    stop_ = true;
  }
  condition_.notify_all();
  // jthreads will join automatically on destruction
}

MemoryArena &ThreadPool::get_local_arena() {
  if (!tls_arena) {
    // Fallback if called outside a worker thread
    tls_arena = std::make_unique<MemoryArena>();
  }
  return *tls_arena;
}

} // namespace vfs
