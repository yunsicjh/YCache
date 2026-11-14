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
struct LfuNodeBase {
  LfuNodeBase *prev{nullptr};
  LfuNodeBase *next{nullptr};

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

  void insertAfter(LfuNodeBase *target) noexcept {
    next = target->next;
    prev = target;
    if (target->next) {
      target->next->prev = this;
    }
    target->next = this;
  }
};

template <typename Key, typename Value> struct LfuNode : public LfuNodeBase {
  Key key;
  Value value;
  std::size_t freq{1};

  LfuNode(Key k, Value v, std::size_t f = 1) noexcept(
      std::is_nothrow_move_constructible_v<Key> &&
      std::is_nothrow_move_constructible_v<Value>)
      : key(std::move(k)), value(std::move(v)), freq(f) {}
};

template <typename Key, typename Value> class FreqList {
public:
  using NodeType = LfuNode<Key, Value>;

  FreqList() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
  }
  FreqList(const FreqList &) = delete;
  FreqList &operator=(const FreqList &) = delete;

  [[nodiscard]] bool empty() const noexcept {
    return dummyHead_.next == &dummyTail_;
  }

  void pushFront(NodeType *node) noexcept { node->insertAfter(&dummyHead_); }
  NodeType *popTail() noexcept {
    if (empty()) {
      return nullptr;
    }
    auto *tailNode = static_cast<NodeType *>(dummyTail_.prev);
    tailNode->unlink();
    return tailNode;
  }

private:
  LfuNodeBase dummyHead_{};
  LfuNodeBase dummyTail_{};
};

template <typename Key, typename Value>
class LfuCache : public YsCachePolicy<Key, Value> {

public:
  using NodeType = LfuNode<Key, Value>;
  using NodePtr = std::unique_ptr<NodeType>;
  using FreqListType = FreqList<Key, Value>;
  using FreqListPtr = std::unique_ptr<FreqListType>;
  using EvictionCallback = typename YsCachePolicy<Key, Value>::EvictionCallback;

  explicit LfuCache(std::size_t capacity, std::size_t maxAvgFreq = 10,
                    EvictionCallback onEvict = nullptr)
      : capacity_(capacity), maxAvgFreq_(maxAvgFreq),
        evictionCallback_(std::move(onEvict)) {}

  LfuCache(const LfuCache &) = delete;
  LfuCache &operator=(const LfuCache &) = delete;
  LfuCache(LfuCache &&) = delete;
  LfuCache &operator=(LfuCache &&) = delete;
  ~LfuCache() override = default;

  void put(Key key, Value value) override {
    if (capacity_ == 0) {
      return;
    }

    std::lock_guard lock(mutex_);

    auto [it, inserted] = keyMap_.try_emplace(std::move(key), nullptr);
    if (!inserted) {
      it->second->value = std::move(value);
      updateFreq(it->second.get());
      return;
    }

    if (keyMap_.size() > capacity_) {
      evictMinFreq();
    }

    try {
      it->second = std::make_unique<NodeType>(it->first, std::move(value), 1);
    } catch (...) {
      keyMap_.erase(it);
      throw;
    }

    minFreq_ = 1;
    auto &list = freqMap_[minFreq_];
    if (!list) {
      list = std::make_unique<FreqListType>();
    }
    list->pushFront(it->second.get());
    curTotalFreq_++;

    if (maxAvgFreq_ > 0 && curTotalFreq_ > maxAvgFreq_ * keyMap_.size()) {
      handleOverMaxAvgFreq();
    }
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = keyMap_.find(key); it != keyMap_.end()) {
      updateFreq(it->second.get());
      return it->second->value;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool remove(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = keyMap_.find(key); it != keyMap_.end()) {
      NodeType *node = it->second.get();
      std::size_t f = node->freq;
      node->unlink();
      curTotalFreq_ -= f;
      // 检查频次桶是否空了
      if (auto itFreq = freqMap_.find(f);
          itFreq != freqMap_.end() && itFreq->second->empty()) {
        freqMap_.erase(itFreq);
      }
      keyMap_.erase(it);
      return true;
    }
    return false;
  }

  void setEvictionCallback(EvictionCallback cb) override {
    std::lock_guard lock(mutex_);
    evictionCallback_ = std::move(cb);
  }

  [[nodiscard]] bool contains(const Key &key) const override {
    std::lock_guard lock(mutex_);
    return keyMap_.find(key) != keyMap_.end();
  }
  [[nodiscard]] std::size_t size() const override {
    std::lock_guard lock(mutex_);
    return keyMap_.size();
  }
  [[nodiscard]] std::size_t capacity() const noexcept override {
    return capacity_;
  }
  [[nodiscard]] bool empty() const override {
    std::lock_guard lock(mutex_);
    return keyMap_.empty();
  }
  void clear() override {
    std::lock_guard lock(mutex_);
    keyMap_.clear();
    freqMap_.clear();
    minFreq_ = 0;
    curTotalFreq_ = 0;
  }

private:
  void updateFreq(NodeType *node) noexcept {
    std::size_t oldFreq = node->freq;
    std::size_t newFreq = oldFreq + 1;
    node->freq = newFreq;

    node->unlink();

    if (auto itOld = freqMap_.find(oldFreq);
        itOld != freqMap_.end() && itOld->second->empty()) {
      freqMap_.erase(itOld);
      if (minFreq_ == oldFreq) {
        minFreq_ = newFreq;
      }
    }

    auto &newList = freqMap_[newFreq];
    if (!newList) {
      newList = std::make_unique<FreqListType>();
    }

    newList->pushFront(node);
    curTotalFreq_++;

    if (maxAvgFreq_ > 0 && curTotalFreq_ > maxAvgFreq_ * keyMap_.size()) {
      handleOverMaxAvgFreq();
    }
  }

  void evictMinFreq() noexcept {
    auto itFreq = freqMap_.find(minFreq_);
    if (itFreq == freqMap_.end()) {
      return;
    }
    NodeType *victim = itFreq->second->popTail();
    if (victim) {
      curTotalFreq_ -= victim->freq;
      if (evictionCallback_) {
        try {
          evictionCallback_(victim->key, victim->value);
        } catch (...) {
          // 忽略回调异常，确保缓存继续运行
        }
      }
      keyMap_.erase(victim->key);
      if (itFreq->second->empty()) {
        freqMap_.erase(itFreq);
      }
    }
  }

  void handleOverMaxAvgFreq() noexcept {
    if (keyMap_.empty()) {
      minFreq_ = 0;
      curTotalFreq_ = 0;
      return;
    }
    std::unordered_map<std::size_t, FreqListPtr> newFreqMap;
    curTotalFreq_ = 0;
    minFreq_ = std::numeric_limits<std::size_t>::max();
    for (auto &[k, nodePtr] : keyMap_) {
      std::size_t newFreq = std::max<std::size_t>(1, nodePtr->freq / 2);
      nodePtr->freq = newFreq;
      curTotalFreq_ += newFreq;
      minFreq_ = std::min(minFreq_, newFreq);
      auto &list = newFreqMap[newFreq];
      if (!list) {
        list = std::make_unique<FreqListType>();
      }
      nodePtr->unlink();
      list->pushFront(nodePtr.get());
    }
    freqMap_ = std::move(newFreqMap);
  }
  std::size_t capacity_{0};
  std::size_t maxAvgFreq_{0};
  std::size_t curTotalFreq_{0};
  std::size_t minFreq_{0};

  mutable std::mutex mutex_;
  std::unordered_map<Key, NodePtr> keyMap_;
  std::unordered_map<std::size_t, FreqListPtr> freqMap_;

  EvictionCallback evictionCallback_;
};

constexpr std::size_t kLfuCacheLineSize = 64;
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedLfuCache : public YsCachePolicy<Key, Value> {
private:
  struct alignas(kLfuCacheLineSize) AlignedShard {
    LfuCache<Key, Value> cache;
    AlignedShard(std::size_t cap, std::size_t maxAvg) : cache(cap, maxAvg) {}
  };

public:
  using EvictionCallback = typename YsCachePolicy<Key, Value>::EvictionCallback;
  explicit ShardedLfuCache(std::size_t capacity, std::size_t sliceNum = 0,
                           std::size_t maxAvgFreq = 10)
      : capacity_(capacity),
        sliceNum_(roundUpToPowerOfTwo(
            sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())) {
    shardMask_ = sliceNum_ - 1;
    std::size_t sliceSize = (capacity_ + sliceNum_ - 1) / sliceNum_;
    shards_.reserve(sliceNum_);
    for (std::size_t i = 0; i < sliceNum_; ++i) {
      shards_.emplace_back(
          std::make_unique<AlignedShard>(sliceSize, maxAvgFreq));
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

  void setEvictionCallback(EvictionCallback cb) override {
    for (auto &shard : shards_) {
      shard->cache.setEvictionCallback(cb);
    }
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
  void purge() { clear(); }

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
  LfuCache<Key, Value> &getShard(const Key &key) {
    std::size_t index = hashFunc_(key) & shardMask_;
    return shards_[index]->cache;
  }
  const LfuCache<Key, Value> &getShard(const Key &key) const {
    std::size_t index = hashFunc_(key) & shardMask_;
    return shards_[index]->cache;
  }
  std::size_t capacity_;
  std::size_t sliceNum_;
  std::size_t shardMask_;
  Hash hashFunc_{};
  std::vector<std::unique_ptr<AlignedShard>> shards_;
};
} // namespace YsCache
