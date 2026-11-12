#pragma once
#include "YsCachePolicy.h"
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
namespace YsCache {
/**
 * @brief 基础 LRU 节点结构体，支持双向链表操作
 *
 */
struct LruNodeBase {
  LruNodeBase *prev{nullptr};
  LruNodeBase *next{nullptr};
  /**
   * @brief 把节点从链表剥离
   *
   *
   */
  void unlink() noexcept {
    if (prev) {
      prev->next = next;
    }
    if (next) {
      next->prev = prev;
    }
    prev = nullptr;
    next = nullptr;
  }
  /**
   * @brief 把节点插入到指定节点之后
   *
   * @param target 指定节点指针
   */
  void insertAfter(LruNodeBase *target) noexcept {
    next = target->next;
    prev = target;
    if (target->next) {
      target->next->prev = this;
    }
    target->next = this;
  }
};

/**
 * @brief 数据节点
 *
 * @tparam Key
 * @tparam Value
 */
template <typename Key, typename Value> struct LruNode : public LruNodeBase {
  Key key;
  Value value;
  LruNode(Key k, Value v) : key(std::move(k)), value(std::move(v)) {}
};

/**
 * @brief 基于 LRU 策略的缓存实现
 *
 * @tparam Key
 * @tparam Value
 */
template <typename Key, typename Value>
class LruCache : public YsCachePolicy<Key, Value> {
public:
  using NodeType = LruNode<Key, Value>;
  using NodePtr = std::unique_ptr<NodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr>;

  explicit LruCache(std::size_t capacity) : capacity_(capacity) {
    initializeList();
  }

  // 显式禁用拷贝与移动
  LruCache(const LruCache &) = delete;
  LruCache &operator=(const LruCache &) = delete;
  LruCache(LruCache &&) = delete;
  LruCache &operator=(LruCache &&) = delete;

  ~LruCache() override {}

  void put(Key key, Value value) override {
    if (capacity_ == 0) {
      return;
    }
    std::lock_guard lock(mutex_);
    auto [it, inserted] = cacheMap_.emplace(std::move(key), nullptr);
    if (!inserted) {
      it->second->value = std::move(value);
      moveToHead(it->second.get());
      return;
    }

    try {
      it->second = std::make_unique<NodeType>(it->first, std::move(value));
    } catch (...) {
      cacheMap_.erase(it);
      throw;
    }

    it->second->insertAfter(&dummyHead_);

    if (cacheMap_.size() > capacity_) {
      evictLeastRecent();
    }
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      moveToHead(it->second.get());
      return it->second->value;
    }
    return std::nullopt;
  }

  [[nodiscard]] virtual bool remove(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      it->second->unlink();
      cacheMap_.erase(it);
      return true;
    }
    return false;
  }
  [[nodiscard]] virtual bool contains(const Key &key) const override {
    std::lock_guard lock(mutex_);
    return cacheMap_.find(key) != cacheMap_.end();
  }

  [[nodiscard]] virtual std::size_t size() const override {
    std::lock_guard lock(mutex_);
    return cacheMap_.size();
  }
  [[nodiscard]] virtual std::size_t capacity() const noexcept override {
    return capacity_;
  }
  [[nodiscard]] virtual bool empty() const override {

    std::lock_guard lock(mutex_);
    return cacheMap_.empty();
  }

  virtual void clear() override {
    std::lock_guard lock(mutex_);
    cacheMap_.clear();
    initializeList();
  }

private:
  void initializeList() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
  }

  void moveToHead(NodeType *node) noexcept {
    node->unlink();
    node->insertAfter(&dummyHead_);
  }

  void evictLeastRecent() noexcept {
    if (dummyTail_.prev == &dummyHead_) {
      return;
    }
    auto *victim = static_cast<NodeType *>(dummyTail_.prev);
    victim->unlink();
    cacheMap_.erase(victim->key);
  }
  std::size_t capacity_;
  mutable std::mutex mutex_;
  NodeMap cacheMap_;
  LruNodeBase dummyHead_;
  LruNodeBase dummyTail_;
};

constexpr std::size_t KCacheLineSize = 64; // 缓存行大小

template <typename Key, typename Value, typename Hash = std::hash<Key>>

class ShardedLruCache : public YsCachePolicy<Key, Value> {
private:
  struct alignas(KCacheLineSize) AlignedShard {
    LruCache<Key, Value> cache;
    AlignedShard(std::size_t capacity) : cache(capacity) {}
  };

public:
  explicit ShardedLruCache(std::size_t capacity, std::size_t shardCount = 0)
      : capacity_(capacity),
        shardCount_(roundUpToPowerOfTwo(
            shardCount > 0 ? shardCount
                           : std::thread::hardware_concurrency())) {
    std::size_t shardCapacity = (capacity_ + shardCount_ - 1) / shardCount_;
    shards_.reserve(shardCount_);
    for (std::size_t i = 0; i < shardCount_; ++i) {
      shards_.emplace_back(std::make_unique<AlignedShard>(shardCapacity));
    }
  }

  void put(Key key, Value value) override {
    getShard(key).put(std::move(key), std::move(value));
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    return getShard(key).get(key);
  }

  [[nodiscard]] bool remove(const Key &key) override {
    return getShard(key).remove(key);
  }

  [[nodiscard]] bool contains(const Key &key) const override {
    return getShard(key).contains(key);
  }

  [[nodiscard]] std::size_t size() const override {
    std::size_t total = 0;
    for (const auto &shard : shards_) {
      total += shard->cache.size();
    }
    return total;
  }

  [[nodiscard]] std::size_t capacity() const noexcept override {
    return capacity_;
  }

  [[nodiscard]] bool empty() const override {
    for (const auto &shard : shards_) {
      if (!shard->cache.empty()) {
        return false;
      }
    }
    return true;
  }

  void clear() override {
    for (auto &shard : shards_) {
      shard->cache.clear();
    }
  }

private:
  static std::size_t roundUpToPowerOfTwo(std::size_t n) noexcept {
    if (n <= 1) {
      return 1;
    }

    std::size_t p = 1;
    while (p < n) {
      p <<= 1;
    }
    return p;
  }

  LruCache<Key, Value> &getShard(const Key &key) {
    std::size_t index = hashFunc_(key) & (shardCount_ - 1);
    return shards_[index]->cache;
  }

  const LruCache<Key, Value> &getShard(const Key &key) const {
    std::size_t index = hashFunc_(key) & (shardCount_ - 1);
    return shards_[index]->cache;
  }

  std::size_t capacity_;
  std::size_t shardCount_;
  Hash hashFunc_{};
  std::vector<std::unique_ptr<AlignedShard>> shards_;
};

template <typename Key, typename Value>
class LruKCache : public YsCachePolicy<Key, Value> {
private:
  struct LruKNode : public LruNodeBase {
    Key key;
    Value value;
    std::size_t accessCount{1};
    bool isInMain{false};
    LruKNode(Key k, Value v, std::size_t count = 1)
        : key(std::move(k)), value(std::move(v)), accessCount(count) {}
  };

  using NodePtr = std::unique_ptr<LruKNode>;
  using NodeMap = std::unordered_map<Key, NodePtr>;

public:
  explicit LruKCache(std::size_t capacity, std::size_t historyCapacity,
                     std::size_t k = 2)
      : k_(k), kCapacity_(capacity), historyCapacity_(historyCapacity) {
    initializeLists();
  }

  // 禁用拷贝与移动
  LruKCache(const LruKCache &) = delete;
  LruKCache &operator=(const LruKCache &) = delete;
  LruKCache(LruKCache &&) = delete;
  LruKCache &operator=(LruKCache &&) = delete;
  ~LruKCache() override = default;

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      LruKNode *node = it->second.get();
      node->accessCount++;

      if (node->isInMain) {
        node->unlink();
        node->insertAfter(&mainHead_);
      } else {
        if (node->accessCount >= k_) {
          node->unlink();
          historySize_--;

          if (mainSize_ >= kCapacity_) {
            evictFromMain();
          }

          node->isInMain = true;
          node->insertAfter(&mainHead_);
          mainSize_++;
        } else {
          node->unlink();
          node->insertAfter(&historyHead_);
        }
      }
      return node->value;
    }
    return std::nullopt;
  }

  void put(Key key, Value value) override {
    if (kCapacity_ == 0 && historyCapacity_ == 0) {
      return;
    }
    std::lock_guard lock(mutex_);

    auto [it, inserted] = cacheMap_.try_emplace(std::move(key), nullptr);
    if (!inserted) {
      LruKNode *node = it->second.get();
      node->value = std::move(value);
      node->accessCount++;

      if (node->isInMain) {
        node->unlink();
        node->insertAfter(&mainHead_);
      } else {
        if (node->accessCount >= k_) {
          node->unlink();
          historySize_--;

          if (mainSize_ >= kCapacity_) {
            evictFromMain();
          }

          node->isInMain = true;
          node->insertAfter(&mainHead_);
          mainSize_++;
        } else {
          node->unlink();
          node->insertAfter(&historyHead_);
        }
      }
      return;
    }

    try {
      it->second = std::make_unique<LruKNode>(it->first, std::move(value), 1);
    } catch (...) {
      cacheMap_.erase(it);
      throw;
    }

    LruKNode *node = it->second.get();
    if (k_ <= 1) {
      if (mainSize_ >= kCapacity_) {
        evictFromMain();
      }
      node->isInMain = true;
      node->insertAfter(&mainHead_);
      mainSize_++;
    } else {
      if (historySize_ >= historyCapacity_) {
        evictFromHistory();
      }
      node->isInMain = false;
      node->insertAfter(&historyHead_);
      historySize_++;
    }
  }

  [[nodiscard]] bool remove(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      LruKNode *node = it->second.get();
      node->unlink();
      if (node->isInMain) {
        mainSize_--;
      } else {
        historySize_--;
      }
      cacheMap_.erase(it);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool contains(const Key &key) const override {
    std::lock_guard lock(mutex_);
    return cacheMap_.find(key) != cacheMap_.end();
  }

  [[nodiscard]] std::size_t size() const override {
    std::lock_guard lock(mutex_);
    return mainSize_ + historySize_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept override {
    return kCapacity_;
  }

  [[nodiscard]] bool empty() const override {
    std::lock_guard lock(mutex_);
    return cacheMap_.empty();
  }

  void clear() override {
    std::lock_guard lock(mutex_);
    cacheMap_.clear();
    mainSize_ = 0;
    historySize_ = 0;
    initializeLists();
  }

private:
  void initializeLists() noexcept {
    mainHead_.next = &mainTail_;
    mainTail_.prev = &mainHead_;
    mainHead_.prev = nullptr;
    mainTail_.next = nullptr;

    historyHead_.next = &historyTail_;
    historyTail_.prev = &historyHead_;
    historyHead_.prev = nullptr;
    historyTail_.next = nullptr;
  }

  void evictFromMain() noexcept {
    if (mainTail_.prev == &mainHead_) {
      return;
    }

    auto *victim = static_cast<LruKNode *>(mainTail_.prev);
    victim->unlink();
    mainSize_--;
    cacheMap_.erase(victim->key);
  }

  void evictFromHistory() noexcept {
    if (historyTail_.prev == &historyHead_) {
      return;
    }

    auto *victim = static_cast<LruKNode *>(historyTail_.prev);
    victim->unlink();
    historySize_--;
    cacheMap_.erase(victim->key);
  }
  std::size_t k_;
  std::size_t kCapacity_;
  std::size_t historyCapacity_;
  std::size_t mainSize_{0};
  std::size_t historySize_{0};

  mutable std::mutex mutex_;
  NodeMap cacheMap_;

  LruNodeBase mainHead_{};
  LruNodeBase mainTail_{};
  LruNodeBase historyHead_{};
  LruNodeBase historyTail_{};
};

} // namespace YsCache
