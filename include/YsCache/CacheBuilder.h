#pragma once
#include "ArcCache.h"
#include "LfuCache.h"
#include "LruCache.h"
#include "MetricsCacheDecorator.h"
#include "TtlCacheDecorator.h"
#include "YsCachePolicy.h"
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
namespace YsCache {

enum class CachePolicyType { LRU, LRU_K, LFU, ARC };

template <typename Key, typename Value> class CacheBuilder {
public:
  using EvictionCallback = typename YsCachePolicy<Key, Value>::EvictionCallback;
  using Duration = std::chrono::milliseconds;
  CacheBuilder() = default;

  // ---------------- Fluent 链式配置方法 ----------------
  CacheBuilder &withCapacity(std::size_t capacity) noexcept {
    capacity_ = capacity;
    return *this;
  }
  CacheBuilder &withPolicy(CachePolicyType policy) noexcept {
    policy_ = policy;
    return *this;
  }
  CacheBuilder &withShards(std::size_t shardCount) noexcept {
    shards_ = shardCount;
    return *this;
  }

  template <typename Rep, typename Period>
  CacheBuilder &withTtl(std::chrono::duration<Rep, Period> ttl) {
    ttl_ = std::chrono::duration_cast<Duration>(ttl);
    return *this;
  }
  CacheBuilder &withMetrics(bool enable = true) noexcept {
    enableMetrics_ = enable;
    return *this;
  }
  CacheBuilder &withEvictionCallback(EvictionCallback cb) {
    evictionCallback_ = std::move(cb);
    return *this;
  }
  // LRU-K 专属参数
  CacheBuilder &withHistoryCapacity(std::size_t historyCap) noexcept {
    historyCapacity_ = historyCap;
    return *this;
  }
  CacheBuilder &withK(std::size_t k) noexcept {
    k_ = k;
    return *this;
  }
  // LFU 专属衰减参数
  CacheBuilder &withMaxAvgFreq(std::size_t maxAvgFreq) noexcept {
    maxAvgFreq_ = maxAvgFreq;
    return *this;
  }

  // ---------------- 终结构建方法 ----------------
  [[nodiscard]] std::unique_ptr<YsCachePolicy<Key, Value>> build() {
    if (capacity_ == 0) {
      throw std::invalid_argument("Cache capacity must be greater than 0");
    }
    std::unique_ptr<YsCachePolicy<Key, Value>> cache;
    // 1. 如果启用了 TTL，底层需要用 TimedValue 实例化，再用 TtlCacheDecorator
    // 包装
    if (ttl_.has_value()) {
      using TimedVal = typename TtlCacheDecorator<Key, Value>::TimedValue;
      auto underlying = createPolicyInstance<TimedVal>(capacity_);
      cache = std::make_unique<TtlCacheDecorator<Key, Value>>(
          std::move(underlying), *ttl_);
    } else {
      cache = createPolicyInstance<Value>(capacity_);
    }
    // 2. 注入淘汰监听回调
    if (evictionCallback_) {
      cache->setEvictionCallback(std::move(evictionCallback_));
    }
    // 3. 如果启用了监控，在最外层包装 MetricsCacheDecorator
    if (enableMetrics_) {
      cache =
          std::make_unique<MetricsCacheDecorator<Key, Value>>(std::move(cache));
    }
    return cache;
  }

private:
  template <typename ValType>
  std::unique_ptr<YsCachePolicy<Key, ValType>>
  createPolicyInstance(std::size_t cap) const {
    if (shards_ > 1) {
      if (policy_ == CachePolicyType::LRU) {
        return std::make_unique<ShardedLruCache<Key, ValType>>(cap, shards_);
      } else if (policy_ == CachePolicyType::LFU) {
        return std::make_unique<ShardedLfuCache<Key, ValType>>(cap, shards_,
                                                               maxAvgFreq_);
      } else {
        throw std::invalid_argument(
            "Sharding is currently only supported for LRU and LFU policies");
      }
    }

    switch (policy_) {
    case CachePolicyType::LRU:
      return std::make_unique<LruCache<Key, ValType>>(cap);
    case CachePolicyType::LFU:
      return std::make_unique<LfuCache<Key, ValType>>(cap, maxAvgFreq_);
    case CachePolicyType::ARC:
      return std::make_unique<ArcCache<Key, ValType>>(cap);
    case CachePolicyType::LRU_K:
      return std::make_unique<LruKCache<Key, ValType>>(
          cap, historyCapacity_ > 0 ? historyCapacity_ : cap, k_);
    }
    return std::make_unique<LruCache<Key, ValType>>(cap);
  }

  std::size_t capacity_{100};
  CachePolicyType policy_{CachePolicyType::LRU};
  std::size_t shards_{0};
  std::optional<Duration> ttl_{std::nullopt};
  bool enableMetrics_{false};
  EvictionCallback evictionCallback_{nullptr};

  // 算法扩展参数
  std::size_t historyCapacity_{0};
  std::size_t k_{2};
  std::size_t maxAvgFreq_{10};
};

} // namespace YsCache