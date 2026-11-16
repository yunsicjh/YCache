#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace YsCache {

// ============================================================================
// 1. 静态监控指标装饰器（Zero-Cost Static Metrics Decorator）
// ============================================================================
/**
 * @brief 编译期静态指标监控包装器（零虚表、零堆分配包装）
 * @tparam CacheType 被包装的底层缓存类型（如 FastLruCache<int, int> 或 StaticTtlDecorator）
 */
template <typename CacheType>
class StaticMetricsDecorator {
public:
  using KeyType = typename CacheType::KeyType;
  using ValueType = typename CacheType::ValueType;
  using key_type = KeyType;
  using mapped_type = ValueType;

  template <typename... Args>
  explicit StaticMetricsDecorator(Args &&...args)
      : cache_(std::forward<Args>(args)...) {}

  StaticMetricsDecorator(const StaticMetricsDecorator &) = delete;
  StaticMetricsDecorator &operator=(const StaticMetricsDecorator &) = delete;
  StaticMetricsDecorator(StaticMetricsDecorator &&) noexcept = default;
  StaticMetricsDecorator &operator=(StaticMetricsDecorator &&) noexcept = default;
  ~StaticMetricsDecorator() = default;

  /**
   * @brief 查询并统计指标（完全内联）
   */
  [[nodiscard]] std::optional<ValueType> get(const KeyType &key) {
    auto res = cache_.get(key);
    if (res.has_value()) {
      hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return res;
  }

  /**
   * @brief 纯读窥视并统计指标
   */
  [[nodiscard]] std::optional<ValueType> peek(const KeyType &key) const {
    auto res = cache_.peek(key);
    if (res.has_value()) {
      hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return res;
  }

  /**
   * @brief 插入或更新并统计写次数
   */
  void put(KeyType key, ValueType value) {
    puts_.fetch_add(1, std::memory_order_relaxed);
    cache_.put(std::move(key), std::move(value));
  }

  /**
   * @brief 删除指定键
   */
  [[nodiscard]] bool remove(const KeyType &key) {
    bool ok = cache_.remove(key);
    if (ok) {
      removes_.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
  }

  [[nodiscard]] bool contains(const KeyType &key) const {
    return cache_.contains(key);
  }

  [[nodiscard]] std::size_t size() const { return cache_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
  [[nodiscard]] bool empty() const { return cache_.empty(); }
  void clear() { cache_.clear(); }

  template <typename Callback>
  void setEvictionCallback(Callback &&cb) {
    cache_.setEvictionCallback(std::forward<Callback>(cb));
  }

  // --- 指标查询接口 ---
  [[nodiscard]] std::size_t hitCount() const noexcept {
    return hits_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t missCount() const noexcept {
    return misses_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t putCount() const noexcept {
    return puts_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t removeCount() const noexcept {
    return removes_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t totalRequests() const noexcept {
    return hitCount() + missCount();
  }

  [[nodiscard]] double hitRate() const noexcept {
    auto total = totalRequests();
    if (total == 0) {
      return 0.0;
    }
    return static_cast<double>(hitCount()) / static_cast<double>(total);
  }

  void resetMetrics() noexcept {
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    puts_.store(0, std::memory_order_relaxed);
    removes_.store(0, std::memory_order_relaxed);
  }

  // 底层缓存穿透访问
  CacheType &underlyingCache() noexcept { return cache_; }
  const CacheType &underlyingCache() const noexcept { return cache_; }

private:
  CacheType cache_;
  mutable std::atomic<std::size_t> hits_{0};
  mutable std::atomic<std::size_t> misses_{0};
  mutable std::atomic<std::size_t> puts_{0};
  mutable std::atomic<std::size_t> removes_{0};
};

// ============================================================================
// 2. 静态 TTL 惰性过期装饰器（Zero-Cost Static TTL Decorator）
// ============================================================================
/**
 * @brief 编译期静态 TTL 缓存包装器（零虚表、零堆分配包装）
 * @tparam Key 键类型
 * @tparam Value 值类型
 * @tparam CacheTemplate 缓存模板（如 FastLruCache、FastArcCache、FastFifoCache）
 * @tparam Clock 时钟源（默认 std::chrono::steady_clock）
 */
template <
    typename Key, typename Value,
    template <typename, typename> class CacheTemplate,
    typename Clock = std::chrono::steady_clock>
class StaticTtlDecorator {
public:
  struct TimedValue {
    Value value;
    typename Clock::time_point expireAt;
  };

  using UnderlyingCache = CacheTemplate<Key, TimedValue>;
  using KeyType = Key;
  using ValueType = Value;
  using key_type = Key;
  using mapped_type = Value;
  using Duration = std::chrono::milliseconds;
  using EvictionCallback = std::function<void(const Key &, const Value &)>;

  template <typename... CacheArgs>
  explicit StaticTtlDecorator(Duration defaultTtl, CacheArgs &&...args)
      : defaultTtl_(defaultTtl), cache_(std::forward<CacheArgs>(args)...) {}

  StaticTtlDecorator(const StaticTtlDecorator &) = delete;
  StaticTtlDecorator &operator=(const StaticTtlDecorator &) = delete;
  StaticTtlDecorator(StaticTtlDecorator &&) noexcept = default;
  StaticTtlDecorator &operator=(StaticTtlDecorator &&) noexcept = default;
  ~StaticTtlDecorator() = default;

  void put(Key key, Value value) {
    putWithTtl(std::move(key), std::move(value), defaultTtl_);
  }

  void putWithTtl(Key key, Value value, Duration ttl) {
    auto expireAt = Clock::now() + ttl;
    cache_.put(std::move(key), TimedValue{std::move(value), expireAt});
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) {
    auto res = cache_.get(key);
    if (!res.has_value()) {
      return std::nullopt;
    }
    if (Clock::now() >= res->expireAt) {
      (void)cache_.remove(key);
      return std::nullopt;
    }
    return std::move(res->value);
  }

  [[nodiscard]] std::optional<Value> peek(const Key &key) const {
    auto res = cache_.peek(key);
    if (!res.has_value()) {
      return std::nullopt;
    }
    if (Clock::now() >= res->expireAt) {
      return std::nullopt;
    }
    return res->value;
  }

  [[nodiscard]] bool contains(const Key &key) const {
    auto res = cache_.peek(key);
    if (!res.has_value()) {
      return false;
    }
    if (Clock::now() >= res->expireAt) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool remove(const Key &key) {
    return cache_.remove(key);
  }

  [[nodiscard]] std::size_t size() const { return cache_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
  [[nodiscard]] bool empty() const { return cache_.empty(); }
  void clear() { cache_.clear(); }

  void setEvictionCallback(EvictionCallback cb) {
    if (cb) {
      cache_.setEvictionCallback(
          [userCb = std::move(cb)](const Key &k, const TimedValue &tv) {
            userCb(k, tv.value);
          });
    } else {
      cache_.setEvictionCallback(nullptr);
    }
  }

  [[nodiscard]] Duration defaultTtl() const noexcept { return defaultTtl_; }

  UnderlyingCache &underlyingCache() noexcept { return cache_; }
  const UnderlyingCache &underlyingCache() const noexcept { return cache_; }

private:
  Duration defaultTtl_;
  UnderlyingCache cache_;
};

} // namespace YsCache
