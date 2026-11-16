// template <typename Key, typename Value>
// class EvictionPolicyConcept {
// public:
//     // 1. 策略自己定义它需要什么指针（LRU 需要 prev/next；LFU 还需要 freq）
//     struct NodeType;

//     // 2. 新节点插入时的排列行为
//     void onInsert(NodeType* node) noexcept;

//     // 3. 节点被查询/命中时的热度更新行为
//     void onAccess(NodeType* node) noexcept;

//     // 4. 节点被显式删除时的摘除行为
//     void onRemove(NodeType* node) noexcept;

//     // 5. 选出最该被淘汰的节点（牺牲品）
//     NodeType* selectVictim() noexcept;

//     // 6. 清空策略内部的数据结构（如哨兵节点重置）
//     void clear() noexcept;
// };

#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace YsCache {

// ============================================================================
// 1. 侵入式链表公共基类（零多余内存填充）
// ============================================================================
struct EvictionNodeBase {
  EvictionNodeBase *prev{nullptr};
  EvictionNodeBase *next{nullptr};
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
  void insertAfter(EvictionNodeBase *target) noexcept {
    next = target->next;
    prev = target;
    if (target->next) {
      target->next->prev = this;
    }
    target->next = this;
  }
};

// ============================================================================
// 2. 策略一：LRU（最近最少使用）淘汰策略
// ============================================================================
template <typename Key, typename Value> class LruEvictionPolicy {
public:
  struct Node : public EvictionNodeBase {
    Key key;
    Value value;
    Node(Key k, Value v) : key(std::move(k)), value(std::move(v)) {}
  };
  using NodeType = Node;

  LruEvictionPolicy() noexcept { initSentinels(); }

  void onInsert(NodeType *node) noexcept { node->insertAfter(&dummyHead_); }
  void onAccess(NodeType *node) noexcept {
    node->unlink();
    node->insertAfter(&dummyHead_);
  }
  void onRemove(NodeType *node) noexcept { node->unlink(); }
  [[nodiscard]] NodeType *selectVictim() noexcept {
    if (dummyTail_.prev == &dummyHead_)
      return nullptr;
    auto *victim = static_cast<NodeType *>(dummyTail_.prev);
    victim->unlink();
    return victim;
  }
  void clear() noexcept { initSentinels(); }

private:
  void initSentinels() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
  }
  EvictionNodeBase dummyHead_{};
  EvictionNodeBase dummyTail_{};
};
// ============================================================================
// 3. 策略二：FIFO（先进先出）淘汰策略
// ============================================================================
template <typename Key, typename Value> class FifoEvictionPolicy {
public:
  struct Node : public EvictionNodeBase {
    Key key;
    Value value;
    Node(Key k, Value v) : key(std::move(k)), value(std::move(v)) {}
  };
  using NodeType = Node;
  FifoEvictionPolicy() noexcept { initSentinels(); }
  void onInsert(NodeType *node) noexcept { node->insertAfter(&dummyHead_); }
  // 核心优化：FIFO 读操作为 0 指令空操作，全内联直接蒸发
  void onAccess(NodeType *) noexcept {}
  void onRemove(NodeType *node) noexcept { node->unlink(); }
  [[nodiscard]] NodeType *selectVictim() noexcept {
    if (dummyTail_.prev == &dummyHead_)
      return nullptr;
    auto *victim = static_cast<NodeType *>(dummyTail_.prev);
    victim->unlink();
    return victim;
  }
  void clear() noexcept { initSentinels(); }

private:
  void initSentinels() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
  }
  EvictionNodeBase dummyHead_{};
  EvictionNodeBase dummyTail_{};
};

// ============================================================================
// 4. 策略三：LFU（最少频率使用 + 动态衰减）淘汰策略
// ============================================================================
template <typename Key, typename Value> class LfuEvictionPolicy {
public:
  struct Node : public EvictionNodeBase {
    Key key;
    Value value;
    std::size_t freq{1};
    Node(Key k, Value v, std::size_t f = 1)
        : key(std::move(k)), value(std::move(v)), freq(f) {}
  };
  using NodeType = Node;
  class FreqList {
  public:
    FreqList() noexcept {
      dummyHead_.next = &dummyTail_;
      dummyTail_.prev = &dummyHead_;
    }
    [[nodiscard]] bool empty() const noexcept {
      return dummyHead_.next == &dummyTail_;
    }
    void pushFront(NodeType *node) noexcept { node->insertAfter(&dummyHead_); }
    NodeType *popTail() noexcept {
      if (empty())
        return nullptr;
      auto *tail = static_cast<NodeType *>(dummyTail_.prev);
      tail->unlink();
      return tail;
    }

  private:
    EvictionNodeBase dummyHead_{};
    EvictionNodeBase dummyTail_{};
  };
  explicit LfuEvictionPolicy(std::size_t maxAvgFreq = 10) noexcept
      : maxAvgFreq_(maxAvgFreq) {}
  void onInsert(NodeType *node) noexcept {
    minFreq_ = 1;
    auto &list = freqMap_[1];
    if (!list)
      list = std::make_unique<FreqList>();
    list->pushFront(node);
    curTotalFreq_++;
  }
  void onAccess(NodeType *node) noexcept {
    std::size_t oldFreq = node->freq;
    std::size_t newFreq = oldFreq + 1;
    node->freq = newFreq;
    node->unlink();
    if (auto it = freqMap_.find(oldFreq);
        it != freqMap_.end() && it->second->empty()) {
      freqMap_.erase(it);
      if (minFreq_ == oldFreq)
        minFreq_ = newFreq;
    }
    auto &newList = freqMap_[newFreq];
    if (!newList)
      newList = std::make_unique<FreqList>();
    newList->pushFront(node);
    curTotalFreq_++;
  }
  void onRemove(NodeType *node) noexcept {
    std::size_t f = node->freq;
    node->unlink();
    curTotalFreq_ = (curTotalFreq_ >= f) ? (curTotalFreq_ - f) : 0;
    if (auto it = freqMap_.find(f);
        it != freqMap_.end() && it->second->empty()) {
      freqMap_.erase(it);
    }
  }
  [[nodiscard]] NodeType *selectVictim() noexcept {
    auto it = freqMap_.find(minFreq_);
    if (it == freqMap_.end())
      return nullptr;
    NodeType *victim = it->second->popTail();
    if (victim) {
      curTotalFreq_ =
          (curTotalFreq_ >= victim->freq) ? (curTotalFreq_ - victim->freq) : 0;
      if (it->second->empty())
        freqMap_.erase(it);
    }
    return victim;
  }
  void clear() noexcept {
    freqMap_.clear();
    minFreq_ = 0;
    curTotalFreq_ = 0;
  }
  void setMaxAvgFreq(std::size_t maxAvg) noexcept { maxAvgFreq_ = maxAvg; }

private:
  std::size_t minFreq_{0};
  std::size_t curTotalFreq_{0};
  std::size_t maxAvgFreq_{10};
  std::unordered_map<std::size_t, std::unique_ptr<FreqList>> freqMap_;
};

// ============================================================================
// 5. 策略四：LRU-K（冷热分级队列）淘汰策略
// ============================================================================
template <typename Key, typename Value> class LruKEvictionPolicy {
public:
  struct Node : public EvictionNodeBase {
    Key key;
    Value value;
    std::size_t accessCount{1};
    bool isInMain{false};
    Node(Key k, Value v, std::size_t count = 1)
        : key(std::move(k)), value(std::move(v)), accessCount(count) {}
  };
  using NodeType = Node;
  explicit LruKEvictionPolicy(std::size_t historyCapacity = 1000,
                              std::size_t k = 2) noexcept
      : historyCapacity_(historyCapacity), k_(k) {
    initLists();
  }
  void onInsert(NodeType *node) noexcept {
    if (k_ <= 1) {
      node->isInMain = true;
      node->insertAfter(&mainHead_);
      mainSize_++;
    } else {
      node->isInMain = false;
      node->insertAfter(&historyHead_);
      historySize_++;
    }
  }
  void onAccess(NodeType *node) noexcept {
    node->accessCount++;
    if (node->isInMain) {
      node->unlink();
      node->insertAfter(&mainHead_);
    } else if (node->accessCount >= k_) {
      // 达到阈值 K，从冷队列晋升到热队列（零堆分配）
      node->unlink();
      historySize_--;
      node->isInMain = true;
      node->insertAfter(&mainHead_);
      mainSize_++;
    } else {
      node->unlink();
      node->insertAfter(&historyHead_);
    }
  }
  void onRemove(NodeType *node) noexcept {
    node->unlink();
    if (node->isInMain) {
      mainSize_ = (mainSize_ > 0) ? mainSize_ - 1 : 0;
    } else {
      historySize_ = (historySize_ > 0) ? historySize_ - 1 : 0;
    }
  }
  [[nodiscard]] NodeType *selectVictim() noexcept {
    // 优先淘汰历史冷队列中的节点（未达到 K 次访问优先牺牲）
    if (historyTail_.prev != &historyHead_) {
      auto *v = static_cast<NodeType *>(historyTail_.prev);
      v->unlink();
      historySize_--;
      return v;
    }
    // 历史队列空时，淘汰热队列最久未访问节点
    if (mainTail_.prev != &mainHead_) {
      auto *v = static_cast<NodeType *>(mainTail_.prev);
      v->unlink();
      mainSize_--;
      return v;
    }
    return nullptr;
  }
  void clear() noexcept {
    initLists();
    mainSize_ = 0;
    historySize_ = 0;
  }
  void setK(std::size_t k) noexcept { k_ = k; }
  void setHistoryCapacity(std::size_t cap) noexcept { historyCapacity_ = cap; }

private:
  void initLists() noexcept {
    mainHead_.next = &mainTail_;
    mainTail_.prev = &mainHead_;
    historyHead_.next = &historyTail_;
    historyTail_.prev = &historyHead_;
  }
  std::size_t historyCapacity_{1000};
  std::size_t k_{2};
  std::size_t mainSize_{0};
  std::size_t historySize_{0};
  EvictionNodeBase mainHead_{};
  EvictionNodeBase mainTail_{};
  EvictionNodeBase historyHead_{};
  EvictionNodeBase historyTail_{};
};
} // namespace YsCache