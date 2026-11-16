#pragma once
#include <mutex>
#include <shared_mutex>
#include <type_traits>

namespace YsCache {

namespace detail {

// 侦测 ConcurrencyPolicy 是否具备 UniqueLock 和 SharedLock
template <typename C, typename = void>
struct is_valid_concurrency_policy : std::false_type {};

template <typename C>
struct is_valid_concurrency_policy<
    C, std::void_t<typename C::UniqueLock, typename C::SharedLock>>
    : std::true_type {};

} // namespace detail
/**
 * @brief 1. 无锁单线程并发策略（Zero-Cost Single-Threaded Policy）
 */

class SingleThreadPolicy {
public:
  void lock() const noexcept {}
  void unlock() const noexcept {}
  void lock_shared() const noexcept {}
  void unlock_shared() const noexcept {}

  struct EmptyLock {
    explicit EmptyLock(const SingleThreadPolicy &) noexcept {}
    ~EmptyLock() noexcept = default;
    EmptyLock(const EmptyLock &) = delete;
    EmptyLock &operator=(const EmptyLock &) = delete;
  };

  using UniqueLock = EmptyLock;
  using SharedLock = EmptyLock;
};
/**
 * @brief 标准互斥锁并发策略（Standard Mutex Policy）
 * 使用 std::mutex 保护所有读写操作，确保基础的多线程安全性。
 */
class StdMutexPolicy {
public:
  void lock() const noexcept { mutex_.lock(); }
  void unlock() const noexcept { mutex_.unlock(); }
  void lock_shared() const noexcept { mutex_.lock(); }
  void unlock_shared() const noexcept { mutex_.unlock(); }

  using UniqueLock = std::unique_lock<const StdMutexPolicy>;
  using SharedLock = std::unique_lock<const StdMutexPolicy>;

private:
  mutable std::mutex mutex_;
};

class SharedMutexPolicy {
public:
  void lock() const { mutex_.lock(); }
  void unlock() const noexcept { mutex_.unlock(); }
  void lock_shared() const { mutex_.lock_shared(); }
  void unlock_shared() const noexcept { mutex_.unlock_shared(); }
  using UniqueLock = std::unique_lock<const SharedMutexPolicy>;
  using SharedLock = std::shared_lock<const SharedMutexPolicy>;

private:
  mutable std::shared_mutex mutex_;
};
} // namespace YsCache