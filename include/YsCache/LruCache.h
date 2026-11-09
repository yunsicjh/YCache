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

// 前向声明
template <typename Key, typename Value> class LruCache;
template <typename Key, typename Value> class ShardedLruCache;
template <typename Key, typename Value> class LruKCache;

template <typename Key, typename Value> class LruNode {
private:
  std::optional<Key> key_;
  std::optional<Value> value_;
  std::shared_ptr<LruNode> next_;
  std::weak_ptr<LruNode> prev_;

public:
  LruNode() = default;
  LruNode(Key key, Value value)
      : key_(std::move(key)), value_(std::move(value)) {}
  friend class LruCache<Key, Value>;
};

template <typename Key, typename Value>
class LruCache : public YsCachePolicy<Key, Value> {
public:
  using NodeType = LruNode<Key, Value>;
  using NodePtr = std::shared_ptr<NodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr>;
  friend class ShardedLruCache<Key, Value>;
  friend class LruKCache<Key, Value>;

  explicit LruCache(std::size_t capacity) : capacity_(capacity) {
    initializeList();
  }

  // 显式禁用拷贝与移动
  LruCache(const LruCache &) = delete;
  LruCache &operator=(const LruCache &) = delete;
  LruCache(LruCache &&) = delete;
  LruCache &operator=(LruCache &&) = delete;

  ~LruCache() override {
    cacheMap_.clear();
    NodePtr cur = dummyHead_;
    while (cur) {
      NodePtr next = cur->next_;
      cur->next_ = nullptr;
      cur->prev_.reset();
      cur = next;
    }
    dummyHead_ = nullptr;
    dummyTail_ = nullptr;
  }

  std::optional<Value> get(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cacheMap_.find(key);
    if (it == cacheMap_.end()) {
      return std::nullopt;
    }
    moveToMostRecent(it->second);
    return it->second->value_;
  }

  void put(Key key, Value value) override {
    if (capacity_ == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cacheMap_.find(key);
    if (it != cacheMap_.end()) {
      it->second->value_ = std::move(value);
      moveToMostRecent(it->second);
    } else {
      if (cacheMap_.size() >= capacity_) {
        evictLeastRecent();
      }
      NodePtr newNode = std::make_shared<NodeType>(key, std::move(value));
      cacheMap_[std::move(key)] = newNode;
      insertNode(newNode);
    }
  }

  bool remove(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cacheMap_.find(key);
    if (it != cacheMap_.end()) {
      removeNode(it->second);
      cacheMap_.erase(it);
      return true;
    }
    return false;
  }

  // 辅助只读接口
  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheMap_.size();
  }

  std::size_t capacity() const noexcept { return capacity_; }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return emptyUnlock();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    clearUnlock();
  }

private:
  std::size_t capacity_;
  NodeMap cacheMap_;
  NodePtr dummyHead_;
  NodePtr dummyTail_;
  mutable std::mutex mutex_;

  void initializeList() {
    dummyHead_ = std::make_shared<NodeType>();
    dummyTail_ = std::make_shared<NodeType>();
    dummyHead_->next_ = dummyTail_;
    dummyTail_->prev_ = dummyHead_;
  }

  void removeNode(NodePtr node) {
    auto prevNode = node->prev_.lock();
    if (prevNode && node->next_) {
      prevNode->next_ = node->next_;
      node->next_->prev_ = prevNode;
      node->next_ = nullptr;
      node->prev_.reset();
    }
  }

  void insertNode(NodePtr node) {
    auto prevNode = dummyTail_->prev_.lock();
    if (!prevNode) {
      return;
    }
    node->next_ = dummyTail_;
    prevNode->next_ = node;
    node->prev_ = prevNode;
    dummyTail_->prev_ = node;
  }

  void evictLeastRecent() {
    if (cacheMap_.empty()) {
      return;
    }
    NodePtr evictNode = dummyHead_->next_;
    if (evictNode && evictNode != dummyTail_ && evictNode->key_.has_value()) {
      removeNode(evictNode);
      cacheMap_.erase(evictNode->key_.value());
    }
  }

  void moveToMostRecent(NodePtr node) {
    if (node->next_ == dummyTail_) {
      return;
    }
    removeNode(node);
    insertNode(node);
  }

  void clearUnlock() {
    cacheMap_.clear();
    NodePtr cur = dummyHead_->next_;
    while (cur && cur != dummyTail_) {
      NodePtr next = cur->next_;
      cur->next_ = nullptr;
      cur->prev_.reset();
      cur = next;
    }
    dummyHead_->next_ = dummyTail_;
    dummyTail_->prev_ = dummyHead_;
  }

  bool emptyUnlock() const { return cacheMap_.empty(); }
};

template <typename Key, typename Value>
class ShardedLruCache : public YsCachePolicy<Key, Value> {
public:
  ShardedLruCache(int capacity, size_t shardCount)
      : capacity_(capacity),
        shardCount_(shardCount > 0 ? shardCount
                                   : std::thread::hardware_concurrency()) {
    size_t count = 1;
    while (count < shardCount_) {
      count = count << 1;
    }
    shardCount_ = count;
    auto shardCapacity =
        std::ceil(static_cast<double>(capacity_) / shardCount_);
    for (auto i = 0; i < shardCount_; ++i) {
      shards_.emplace_back(
          std::make_unique<LruCache<Key, Value>>(shardCapacity));
    }
  }

  std::optional<Value> get(const Key &key) override {
    auto &shard = getShard(key);
    return shard.get(key);
  }

  void put(Key key, Value value) override {
    auto &shard = getShard(key);
    shard.put(std::move(key), std::move(value));
  }

  bool remove(const Key &key) override {
    auto &shard = getShard(key);
    return shard.remove(key);
  }

  void clear() {
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(shardCount_);
    for (auto i = 0; i < shardCount_; ++i) {
      locks.emplace_back(shards_[i]->mutex_);
      shards_[i]->clearUnlock();
    }
  }

  std::size_t size() const {
    std::size_t totalSize = 0;
    for (const auto &shard : shards_) {
      totalSize += shard->size();
    }
    return totalSize;
  }

  std::size_t capacity() const {
    std::size_t totalCapacity = 0;
    for (const auto &shard : shards_) {
      totalCapacity += shard->capacity();
    }
    return totalCapacity;
  }

  bool empty() const {
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(shardCount_);
    for (const auto &shard : shards_) {
      locks.emplace_back(shard->mutex_);
      if (!shard->emptyUnlock()) {
        return false;
      }
    }
    return true;
  }

private:
  /*TODO：
  #include <new>

  struct alignas(std::hardware_destructive_interference_size) AlignedShard {
      LruCache<Key, Value> cache;
  };

  // 分片数组紧凑存放时，每个元素都会独占至少一个缓存行
  std::vector<AlignedShard> shards_;
   */
  int capacity_;
  size_t shardCount_;
  std::vector<std::unique_ptr<LruCache<Key, Value>>> shards_;

  LruCache<Key, Value> &getShard(const Key &key) {
    auto hashValue = std::hash<Key>{}(key);
    auto index = hashValue & (shardCount_ - 1);
    return *shards_[index];
  }
};

template <typename Key, typename Value>
class LruKCache : public YsCachePolicy<Key, Value> {
private:
  struct HistoryEntry {
    std::size_t accessCount_{1};
    Value value_;
  };
  using MainCache = LruCache<Key, Value>;
  using HistoryCache = LruCache<Key, HistoryEntry>;

public:
  LruKCache(std::size_t capacity, std::size_t historyCapacity,
            std::size_t k = 2)
      : k_(k), kCache_(std::make_unique<MainCache>(capacity)),
        historyList_(std::make_unique<HistoryCache>(historyCapacity)) {}

  std::optional<Value> get(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto kCacheValue = kCache_->get(key);
    if (kCacheValue.has_value()) {
      return kCacheValue;
    }

    auto hisOpt = historyList_->get(key);
    if (hisOpt.has_value()) {
      auto hisEntry = std::move(hisOpt.value());
      hisEntry.accessCount_ += 1;
      Value val = hisEntry.value_;
      if (hisEntry.accessCount_ >= k_) {
        historyList_->remove(key);
        kCache_->put(key, std::move(hisEntry.value_));

      } else {
        historyList_->put(key, std::move(hisEntry));
      }
      return val;
    }
    return std::nullopt;
  }

  void put(Key key, Value value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (kCache_->get(key).has_value()) {
      kCache_->put(std::move(key), std::move(value));
      return;
    }
    auto hisOpt = historyList_->get(key);
    if (hisOpt.has_value()) {
      auto hisEntry = std::move(hisOpt.value());
      hisEntry.accessCount_ += 1;

      if (hisEntry.accessCount_ >= k_) {
        historyList_->remove(key);
        kCache_->put(std::move(key), std::move(value));
        return;
      } else {
        hisEntry.value_ = std::move(value);
        historyList_->put(std::move(key), std::move(hisEntry));
        return;
      }
    }

    if (k_ <= 1) {
      kCache_->put(std::move(key), std::move(value));
    } else {
      historyList_->put(std::move(key), HistoryEntry{1, std::move(value)});
    }
  }

  bool remove(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bool removedFromKCache = kCache_->remove(key);
    bool removedFromHistory = historyList_->remove(key);
    return removedFromKCache || removedFromHistory;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    kCache_->clear();
    historyList_->clear();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kCache_->size() + historyList_->size();
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kCache_->empty() && historyList_->empty();
  }

private:
  std::size_t k_;
  std::unique_ptr<MainCache> kCache_;
  std::unique_ptr<HistoryCache> historyList_;
  mutable std::mutex mutex_;
};

} // namespace YsCache
