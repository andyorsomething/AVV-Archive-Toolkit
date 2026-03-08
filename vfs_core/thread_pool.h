/**
 * @file thread_pool.h
 * @brief Lightweight worker pool and thread-local scratch allocator used by
 * archive I/O.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>


namespace vfs {

/**
 * @class MemoryArena
 * @brief Simple bump allocator used for per-task temporary buffers.
 */
class MemoryArena {
public:
  /// @brief Creates an arena with an initial capacity.
  /// @param capacity Initial backing storage in bytes.
  explicit MemoryArena(size_t capacity = 1024 * 1024 * 16); // 16MB default
  ~MemoryArena();

  /// @brief Non-copyable because arenas are intended to stay thread-local.
  MemoryArena(const MemoryArena &) = delete;
  MemoryArena &operator=(const MemoryArena &) = delete;

  /// @brief Allocates aligned storage from the arena, growing if needed.
  /// @param size Requested size in bytes.
  /// @param alignment Requested byte alignment.
  /// @return Pointer to writable storage owned by the arena.
  void *allocate(size_t size, size_t alignment = 16);
  /// @brief Rewinds the arena so temporary storage can be reused.
  void reset();

private:
  std::vector<char> buffer_;
  size_t offset_ = 0;
};

/**
 * @class ThreadPool
 * @brief Minimal fixed-size worker pool for asynchronous archive operations.
 */
class ThreadPool {
public:
  /// @brief Creates a worker pool.
  /// @param threads Requested worker count; `0` is normalized to one worker.
  explicit ThreadPool(size_t threads = std::thread::hardware_concurrency());
  ~ThreadPool();

  /// @brief Queues work on the pool.
  /// @tparam F Callable type.
  /// @tparam Args Argument types.
  /// @param f Callable to execute.
  /// @param args Arguments forwarded into the callable.
  /// @return Future for the callable result.
  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<std::invoke_result_t<F, Args...>>;

  /// @brief Returns the thread-local scratch arena for the calling worker.
  static MemoryArena &get_local_arena();

private:
  std::vector<std::jthread> workers_;
  std::queue<std::function<void()>> tasks_;

  std::mutex queue_mutex_;
  std::condition_variable condition_;
  bool stop_ = false;
};

/// @cond INTERNAL
template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<std::invoke_result_t<F, Args...>> {
  using return_type = std::invoke_result_t<F, Args...>;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // don't allow enqueueing after stopping the pool
    if (stop_)
      throw std::runtime_error("enqueue on stopped ThreadPool");

    tasks_.emplace([task]() { (*task)(); });
  }
  condition_.notify_one();
  return res;
}
/// @endcond

} // namespace vfs
