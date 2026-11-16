#pragma once
#include "ArcCache.h"
#include "StaticDecorators.h"
#include "policies/ConcurrencyPolicy.h"
#include "policies/EvictionPolicy.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace YsCache {

namespace detail {

// 侦测 EvictionPolicy 是否具备完备的 5 大生命周期接口
template <typename Policy, typename Node, typename = void>
struct is_valid_eviction_policy : std::false_type {};

template <typename Policy, typename Node>
struct is_valid_eviction_policy<
    Policy, Node,
    std::void_t<
        decltype(std::declval<Policy &>().onInsert(std::declval<Node *>())),
        decltype(std::declval<Policy &>().onAccess(std::declval<Node *>())),
        decltype(std::declval<Policy &>().onRemove(std::declval<Node *>())),
        decltype(std::declval<Policy &>().selectVictim()),
        decltype(std::declval<Policy &>().clear())>> : std::true_type {};

} // namespace detail
/**
 * @brief 零成本抽象静态策略缓存宿主类（Zero-Cost Policy-Based Cache）
 * @tparam Key 键类型
 * @tparam Value 值类型
 * @tparam EvictionPolicy 淘汰策略模板（默认 LRU）
 * @tparam ConcurrencyPolicy 并发策略（默认 StdMutexPolicy）
 * @tparam Hash 哈希算法
 *
 */
template <
    typename Key, typename Value,
    template <typename, typename> class EvictionPolicy = LruEvictionPolicy,
    typename ConcurrencyPolicy = StdMutexPolicy, typename Hash = std::hash<Key>>
class PolicyCache : private ConcurrencyPolicy {
public:
  using KeyType = Key;
  using ValueType = Value;
  using key_type = Key;
  using mapped_type = Value;
  using Policy = EvictionPolicy<Key, Value>;
  using NodeType = typename Policy::NodeType;
  using NodePtr = std::unique_ptr<NodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr, Hash>;
  using EvictionCallback = std::function<void(const Key &, const Value &)>;

  // ---------------- 编译期策略契约防御（C++17 Static Assertions） ----------------
  static_assert(
      detail::is_valid_concurrency_policy<ConcurrencyPolicy>::value,
      "[YsCache Error] ConcurrencyPolicy must provide UniqueLock and SharedLock types.");

  static_assert(
      detail::is_valid_eviction_policy<Policy, NodeType>::value,
      "[YsCache Error] EvictionPolicy must implement: onInsert(Node*), onAccess(Node*), "
      "onRemove(Node*), selectVictim(), and clear() methods.");

  static_assert(
      std::is_same_v<decltype(std::declval<Policy &>().selectVictim()), NodeType *>,
      "[YsCache Error] EvictionPolicy::selectVictim() must return NodeType*.");

  // 基础构造
  explicit PolicyCache(std::size_t capacity, EvictionCallback onEvict = nullptr)
      : capacity_(capacity), evictionCallback_(std::move(onEvict)) {
    if (capacity_ == 0) {
      throw std::invalid_argument("Capacity must be greater than 0");
    }
  }

  // 完美转发构造：支持给策略传递专用参数
  template <typename... PolicyArgs>
  PolicyCache(std::size_t capacity, PolicyArgs &&...args)
      : capacity_(capacity),
        evictionPolicy_(std::forward<PolicyArgs>(args)...) {
    if (capacity_ == 0) {
      throw std::invalid_argument("Capacity must be greater than 0");
    }
  }

  // 禁用拷贝，允许移动
  PolicyCache(const PolicyCache &) = delete;
  PolicyCache &operator=(const PolicyCache &) = delete;
  PolicyCache(PolicyCache &&) noexcept = default;
  PolicyCache &operator=(PolicyCache &&) noexcept = default;
  ~PolicyCache() = default; // 非虚析构函数：零虚表开销

  /**
   * @brief 插入或更新键值（Sink 传参模式）
   */
  void put(Key key, Value value) {
    if (capacity_ == 0) {
      return;
    }
    typename ConcurrencyPolicy::UniqueLock lock(*this);
    auto [it, inserted] = keyMap_.try_emplace(key, nullptr);
    if (!inserted) {
      it->second->value = std::move(value);
      evictionPolicy_.onAccess(it->second.get());
      return;
    }
    // 触发超限淘汰
    if (keyMap_.size() > capacity_) {
      NodeType *victim = evictionPolicy_.selectVictim();
      if (victim) {
        if (evictionCallback_) {
          try {
            evictionCallback_(victim->key, victim->value);
          } catch (...) {
            // 异常隔离防线
          }
        }
        keyMap_.erase(victim->key);
      }
    }
    try {
      it->second = std::make_unique<NodeType>(it->first, std::move(value));
    } catch (...) {
      keyMap_.erase(it);
      throw;
    }
    evictionPolicy_.onInsert(it->second.get());
  }

  /**
   * @brief 读取并标记热度（常规读）
   */
  [[nodiscard]] std::optional<Value> get(const Key &key) {
    typename ConcurrencyPolicy::UniqueLock lock(*this);
    if (auto it = keyMap_.find(key); it != keyMap_.end()) {
      evictionPolicy_.onAccess(it->second.get());
      return it->second->value;
    }
    return std::nullopt;
  }

  /**
   * @brief 纯读窥视（Peek）：不改变热度排序，支持 SharedLock 读锁并发并行！
   */
  [[nodiscard]] std::optional<Value> peek(const Key &key) const {
    typename ConcurrencyPolicy::SharedLock lock(*this);
    if (auto it = keyMap_.find(key); it != keyMap_.end()) {
      return it->second->value;
    }
    return std::nullopt;
  }

  /**
   * @brief 显式删除指定键
   */
  [[nodiscard]] bool remove(const Key &key) {
    typename ConcurrencyPolicy::UniqueLock lock(*this);
    if (auto it = keyMap_.find(key); it != keyMap_.end()) {
      evictionPolicy_.onRemove(it->second.get());
      keyMap_.erase(it);
      return true;
    }
    return false;
  }
  /**
   * @brief 查询键是否存在（不修改热度）
   */
  [[nodiscard]] bool contains(const Key &key) const {
    typename ConcurrencyPolicy::SharedLock lock(*this);
    return keyMap_.find(key) != keyMap_.end();
  }
  [[nodiscard]] std::size_t size() const {
    typename ConcurrencyPolicy::SharedLock lock(*this);
    return keyMap_.size();
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const {
    typename ConcurrencyPolicy::SharedLock lock(*this);
    return keyMap_.empty();
  }
  void clear() {
    typename ConcurrencyPolicy::UniqueLock lock(*this);
    keyMap_.clear();
    evictionPolicy_.clear();
  }
  void setEvictionCallback(EvictionCallback cb) {
    typename ConcurrencyPolicy::UniqueLock lock(*this);
    evictionCallback_ = std::move(cb);
  }

  // 策略访问器（用于运行时调优参数，例如 LFU 的 maxAvgFreq）
  Policy &evictionPolicy() noexcept { return evictionPolicy_; }
  const Policy &evictionPolicy() const noexcept { return evictionPolicy_; }

private:
  std::size_t capacity_;
  NodeMap keyMap_;
  Policy evictionPolicy_{};
  EvictionCallback evictionCallback_{nullptr};
};

// ============================================================================
// 常用开箱即用的工业级类型别名（Type Aliases）
// ============================================================================

// 单线程极限性能版（0 虚表、0 锁开销）
template <typename Key, typename Value>
using FastLruCache = PolicyCache<Key, Value, LruEvictionPolicy, SingleThreadPolicy>;

template <typename Key, typename Value>
using FastFifoCache = PolicyCache<Key, Value, FifoEvictionPolicy, SingleThreadPolicy>;

template <typename Key, typename Value>
using FastLfuCache = PolicyCache<Key, Value, LfuEvictionPolicy, SingleThreadPolicy>;

template <typename Key, typename Value>
using FastLruKCache = PolicyCache<Key, Value, LruKEvictionPolicy, SingleThreadPolicy>;

// 多线程互斥安全版
template <typename Key, typename Value>
using ConcurrentLruCache = PolicyCache<Key, Value, LruEvictionPolicy, StdMutexPolicy>;

template <typename Key, typename Value>
using ConcurrentLfuCache = PolicyCache<Key, Value, LfuEvictionPolicy, StdMutexPolicy>;

} // namespace YsCache