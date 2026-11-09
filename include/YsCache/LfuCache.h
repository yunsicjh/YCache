#pragma once
#include "YsCachePolicy.h"
#include <climits>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace YsCache {

template <typename Key, typename Value> class LfuCache;

template <typename Key, typename Value> class FreqList {
private:
  struct LfuNode {
    int freq{1};
    std::optional<Key> key;
    std::optional<Value> value;
    std::weak_ptr<LfuNode> prev;
    std::shared_ptr<LfuNode> next;

    LfuNode() = default;
    LfuNode(int f, Key k, Value v)
        : freq(f), key(std::move(k)), value(std::move(v)) {}
  };

public:
  using NodeType = LfuNode;
  using NodePtr = std::shared_ptr<NodeType>;
  friend class LfuCache<Key, Value>;

  FreqList() { initializeList(); }

  ~FreqList() {
    // 迭代断开链表，防止深层递归析构导致栈溢出
    NodePtr cur = dummyHead_;
    while (cur) {
      NodePtr next = cur->next;
      cur->next = nullptr;
      cur->prev.reset();
      cur = next;
    }
    dummyHead_ = nullptr;
    dummyTail_ = nullptr;
  }

  void removeNode(NodePtr node) {
    auto prevNode = node->prev.lock();
    if (prevNode && node->next) {
      prevNode->next = node->next;
      node->next->prev = prevNode;
      node->prev.reset();
      node->next = nullptr;
    }
  }

  NodePtr popTail() {
    auto resNode = dummyTail_->prev.lock();
    if (resNode && resNode != dummyHead_) {
      removeNode(resNode);
      return resNode;
    }
    return nullptr;
  }

  void pushFront(NodePtr node) {
    node->next = dummyHead_->next;
    dummyHead_->next = node;
    node->next->prev = node;
    node->prev = dummyHead_;
  }

  bool empty() const { return dummyHead_->next == dummyTail_; }

private:
  NodePtr dummyHead_;
  NodePtr dummyTail_;

  void initializeList() {
    dummyHead_ = std::make_shared<NodeType>();
    dummyTail_ = std::make_shared<NodeType>();
    dummyHead_->next = dummyTail_;
    dummyTail_->prev = dummyHead_;
  }
};

template <typename Key, typename Value>
class LfuCache : public YsCachePolicy<Key, Value> {
public:
  using NodeType = typename FreqList<Key, Value>::NodeType;
  using NodePtr = typename FreqList<Key, Value>::NodePtr;
  using FreqListPtr = std::unique_ptr<FreqList<Key, Value>>;

  explicit LfuCache(size_t capacity, size_t maxAvgFreq = 10)
      : capacity_(capacity), maxAvgFreq_(maxAvgFreq) {}

  LfuCache(const LfuCache &) = delete;
  LfuCache &operator=(const LfuCache &) = delete;
  LfuCache(LfuCache &&) = delete;
  LfuCache &operator=(LfuCache &&) = delete;

  ~LfuCache() override { clear(); }

  void put(Key key, Value value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0) {
      return;
    }
    auto it = keyMap_.find(key);
    if (it != keyMap_.end()) {
      auto nodePtr = it->second;
      nodePtr->value = std::move(value);
      updateFreq(nodePtr);
      return;
    }

    if (size_ >= capacity_) {
      auto itFreq = freqMap_.find(minFreq_);
      if (itFreq != freqMap_.end()) {
        auto nodeToRemove = itFreq->second->popTail();
        if (nodeToRemove) {
          keyMap_.erase(nodeToRemove->key.value());
          curTotalFreq_ -= nodeToRemove->freq;
          size_--;
          if (itFreq->second->empty()) {
            freqMap_.erase(itFreq);
          }
        }
      }
    }
    NodePtr newNode =
        std::make_shared<NodeType>(1, std::move(key), std::move(value));

    minFreq_ = 1;
    auto &freqList = freqMap_[minFreq_];
    if (!freqList) {
      freqList = std::make_unique<FreqList<Key, Value>>();
    }
    freqList->pushFront(newNode);
    curTotalFreq_ += 1;
    keyMap_[newNode->key.value()] = newNode;
    size_++;
  }

  std::optional<Value> get(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = keyMap_.find(key);
    if (it == keyMap_.end()) {
      return std::nullopt;
    }
    auto node = it->second;
    updateFreq(node);
    return node->value;
  }

  bool remove(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = keyMap_.find(key);
    if (it == keyMap_.end()) {
      return false;
    }

    auto node = it->second;
    int freq = node->freq;
    auto itFreq = freqMap_.find(freq);
    if (itFreq != freqMap_.end()) {
      itFreq->second->removeNode(node);
      curTotalFreq_ -= freq;
      keyMap_.erase(it);
      size_--;

      if (itFreq->second->empty()) {
        freqMap_.erase(itFreq);

        if (size_ == 0) {
          minFreq_ = 0;
        } else if (freq == minFreq_) {
          int minVal = INT_MAX;
          for (const auto &pair : freqMap_) {
            minVal = std::min(minVal, pair.first);
          }
          minFreq_ = minVal;
        }
      }
      return true;
    }
    return false;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    keyMap_.clear();
    freqMap_.clear();
    size_ = 0;
    curTotalFreq_ = 0;
    minFreq_ = 0;
  }

  void purge() { clear(); }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  size_t capacity() const noexcept { return capacity_; }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ == 0;
  }

private:
  void updateFreq(NodePtr node) {
    int oldFreq = node->freq;
    int newFreq = oldFreq + 1;
    node->freq = newFreq;

    auto itOld = freqMap_.find(oldFreq);
    if (itOld != freqMap_.end()) {
      itOld->second->removeNode(node);
      if (itOld->second->empty()) {
        freqMap_.erase(itOld);
        if (minFreq_ == oldFreq) {
          minFreq_++;
        }
      }
    }

    auto &newList = freqMap_[newFreq];
    if (!newList) {
      newList = std::make_unique<FreqList<Key, Value>>();
    }
    newList->pushFront(node);
    curTotalFreq_ += 1;

    // 完成新桶挂载后再检测老化衰减，防止空指针异常
    if (maxAvgFreq_ > 0 && curTotalFreq_ > maxAvgFreq_ * size_) {
      handleOverMaxAvgFreq();
    }
  }

  void handleOverMaxAvgFreq() {
    if (keyMap_.empty()) {
      minFreq_ = 0;
      curTotalFreq_ = 0;
      return;
    }

    std::unordered_map<int, FreqListPtr> newFreqMap;
    curTotalFreq_ = 0;
    minFreq_ = INT_MAX;

    for (auto &[key, node] : keyMap_) {
      int newFreq = std::max(1, node->freq / 2);
      node->freq = newFreq;

      curTotalFreq_ += newFreq;
      minFreq_ = std::min(minFreq_, newFreq);

      auto &list = newFreqMap[newFreq];
      if (!list) {
        list = std::make_unique<FreqList<Key, Value>>();
      }
      list->pushFront(node);
    }

    freqMap_ = std::move(newFreqMap);
  }

  int minFreq_{0};
  size_t capacity_{0};
  size_t size_{0};
  size_t maxAvgFreq_{0};
  size_t curTotalFreq_{0};
  std::unordered_map<Key, NodePtr> keyMap_;
  std::unordered_map<int, FreqListPtr> freqMap_;
  mutable std::mutex mutex_;
};

// ============================================================================
// 分片高并发 LFU 缓存（ShardedLfuCache / KHashLfuCache）
// ============================================================================
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedLfuCache : public YsCachePolicy<Key, Value> {
public:
  ShardedLfuCache(size_t capacity, size_t sliceNum = 0, size_t maxAverageNum = 10)
      : capacity_(capacity) {
    size_t hw = std::max(1u, std::thread::hardware_concurrency());
    size_t targetShards = sliceNum > 0 ? sliceNum : hw;

    // 向上取最近的 2 的幂次，便于做快速位与运算 (& (shardCount_ - 1))
    shardCount_ = 1;
    while (shardCount_ < targetShards) {
      shardCount_ <<= 1;
    }

    size_t sliceSize =
        static_cast<size_t>(std::ceil(static_cast<double>(capacity_) / shardCount_));

    lfuSliceCaches_.reserve(shardCount_);
    for (size_t i = 0; i < shardCount_; ++i) {
      lfuSliceCaches_.emplace_back(
          std::make_unique<LfuCache<Key, Value>>(sliceSize, maxAverageNum));
    }
  }

  void put(Key key, Value value) override {
    getShard(key).put(std::move(key), std::move(value));
  }

  std::optional<Value> get(const Key &key) override {
    return getShard(key).get(key);
  }

  bool remove(const Key &key) override {
    return getShard(key).remove(key);
  }

  void clear() {
    for (auto &shard : lfuSliceCaches_) {
      shard->clear();
    }
  }

  // 兼容 kamacache 的 purge 命名习惯
  void purge() { clear(); }

  size_t size() const {
    size_t total = 0;
    for (const auto &shard : lfuSliceCaches_) {
      total += shard->size();
    }
    return total;
  }

  size_t capacity() const {
    size_t total = 0;
    for (const auto &shard : lfuSliceCaches_) {
      total += shard->capacity();
    }
    return total;
  }

  bool empty() const {
    for (const auto &shard : lfuSliceCaches_) {
      if (!shard->empty()) {
        return false;
      }
    }
    return true;
  }

private:
  size_t capacity_;
  size_t shardCount_;
  std::vector<std::unique_ptr<LfuCache<Key, Value>>> lfuSliceCaches_;
  Hash hashFunc_;

  LfuCache<Key, Value> &getShard(const Key &key) {
    return *lfuSliceCaches_[hashFunc_(key) & (shardCount_ - 1)];
  }
};

// 兼容 kamacache 的别名
template <typename Key, typename Value>
using KHashLfuCache = ShardedLfuCache<Key, Value>;

template <typename Key, typename Value>
using HashLfuCache = ShardedLfuCache<Key, Value>;

} // namespace YsCache
