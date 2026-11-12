#pragma once
#include "YsCachePolicy.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace YsCache {

// 标识节点当前归属的链表
enum class ArcListType { T1, B1, T2, B2 };

struct ArcNodeBase {
  ArcNodeBase *prev{nullptr};
  ArcNodeBase *next{nullptr};

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
  void insertAfter(ArcNodeBase *target) noexcept {
    next = target->next;
    prev = target;
    if (target->next) {
      target->next->prev = this;
    }
    target->next = this;
  }
};

template <typename Key, typename Value> struct ArcNode : public ArcNodeBase {
  Key key;
  std::optional<Value> value;
  ArcListType type{ArcListType::T1};

  ArcNode(Key k, Value v, ArcListType t)
      : key(std::move(k)), value(std::move(v)), type(t) {}

  ArcNode(Key k, ArcListType t)
      : key(std::move(k)), value(std::nullopt), type(t) {}
};

template <typename Key, typename Value> class ArcList {
public:
  using NodeType = ArcNode<Key, Value>;

  ArcList() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
  }

  ArcList(const ArcList &) = delete;
  ArcList &operator=(const ArcList &) = delete;

  void clear() noexcept {
    dummyHead_.next = &dummyTail_;
    dummyTail_.prev = &dummyHead_;
    dummyHead_.prev = nullptr;
    dummyTail_.next = nullptr;
    size_ = 0;
  }

  [[nodiscard]] bool empty() const noexcept {
    return dummyHead_.next == &dummyTail_;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  void pushFront(NodeType *node) noexcept {
    node->insertAfter(&dummyHead_);
    size_++;
  }

  void removeNode(NodeType *node) noexcept {
    node->unlink();
    size_--;
  }

  NodeType *popTail() noexcept {
    if (empty()) {
      return nullptr;
    }

    auto *tailNode = static_cast<NodeType *>(dummyTail_.prev);
    tailNode->unlink();
    size_--;
    return tailNode;
  }

private:
  ArcNodeBase dummyHead_{};
  ArcNodeBase dummyTail_{};
  std::size_t size_{0};
};

template <typename Key, typename Value>
class ArcCache : public YsCachePolicy<Key, Value> {
public:
  using NodeType = ArcNode<Key, Value>;
  using NodePtr = std::unique_ptr<NodeType>;
  using ListType = ArcList<Key, Value>;

  explicit ArcCache(std::size_t capacity) : capacity_(capacity), p_(0) {}
  ArcCache(const ArcCache &) = delete;
  ArcCache &operator=(const ArcCache &) = delete;
  ArcCache(ArcCache &&) = delete;
  ArcCache &operator=(ArcCache &&) = delete;
  ~ArcCache() override = default;

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      NodeType *node = it->second.get();
      // 仅命中真实缓存区（T1 或 T2）时返回数据，并提拔到 T2
      if (node->type == ArcListType::T1) {
        t1_.removeNode(node);
        node->type = ArcListType::T2;
        t2_.pushFront(node);
        return node->value;
      } else if (node->type == ArcListType::T2) {
        t2_.removeNode(node);
        t2_.pushFront(node);
        return node->value;
      }
      // 命中 B1 或 B2 属于幽灵未命中（Ghost Hit），仅记录历史，无实际值
      return std::nullopt;
    }
    return std::nullopt;
  }

  void put(Key key, Value value) override {
    if (capacity_ == 0) {
      return;
    }
    std::lock_guard lock(mutex_);
    // 核心优化：利用 try_emplace 单次哈希
    auto [it, inserted] = cacheMap_.try_emplace(key, nullptr);
    if (!inserted) {
      // 键已在元数据中存在（可能在 T1, T2, B1, B2）
      NodeType *node = it->second.get();
      // Case 1: 命中 T1 或 T2（真实有效缓存更新）
      if (node->type == ArcListType::T1 || node->type == ArcListType::T2) {
        node->value = std::move(value);
        if (node->type == ArcListType::T1) {
          t1_.removeNode(node);
        } else {
          t2_.removeNode(node);
        }
        node->type = ArcListType::T2;
        t2_.pushFront(node);
        return;
      }
      // Case 2: 命中 B1 幽灵队列（增加 p，偏向近期访问）
      if (node->type == ArcListType::B1) {
        std::size_t b1Size = b1_.size();
        std::size_t b2Size = b2_.size();
        std::size_t delta = (b1Size >= b2Size) ? 1 : (b2Size / b1Size);
        p_ = std::min(capacity_, p_ + delta);
        replace(false);
        b1_.removeNode(node);
        node->value = std::move(value); // 填入真实实体
        node->type = ArcListType::T2;
        t2_.pushFront(node);
        return;
      }
      // Case 3: 命中 B2 幽灵队列（减少 p，偏向高频访问）
      if (node->type == ArcListType::B2) {
        std::size_t b1Size = b1_.size();
        std::size_t b2Size = b2_.size();
        std::size_t delta = (b2Size >= b1Size) ? 1 : (b1Size / b2Size);
        p_ = (p_ > delta) ? (p_ - delta) : 0;
        replace(true);
        b2_.removeNode(node);
        node->value = std::move(value); // 填入真实实体
        node->type = ArcListType::T2;
        t2_.pushFront(node);
        return;
      }
    }
    // Case 4: 全新键首次进入（完全未命中）
    std::size_t totalL1 = t1_.size() + b1_.size();
    std::size_t totalAll = totalL1 + t2_.size() + b2_.size();
    // 内存元数据超限清理
    if (totalL1 == capacity_) {
      if (t1_.size() < capacity_) {
        auto *victim = b1_.popTail();
        if (victim) {
          cacheMap_.erase(victim->key);
        }
        replace(false);
      } else {
        auto *victim = t1_.popTail();
        if (victim) {
          cacheMap_.erase(victim->key);
        }
      }
    } else if (totalL1 < capacity_) {
      if (totalAll >= 2 * capacity_) {
        auto *victim = b2_.popTail();
        if (victim) {
          cacheMap_.erase(victim->key);
        }
      }
      replace(false);
    }
    // 构造新节点并加入 T1（强异常安全保证）
    try {
      it->second = std::make_unique<NodeType>(it->first, std::move(value),
                                              ArcListType::T1);
    } catch (...) {
      cacheMap_.erase(it);
      throw;
    }
    t1_.pushFront(it->second.get());
  }

  [[nodiscard]] bool remove(const Key &key) override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      NodeType *node = it->second.get();
      switch (node->type) {
      case ArcListType::T1:
        t1_.removeNode(node);
        break;
      case ArcListType::B1:
        b1_.removeNode(node);
        break;
      case ArcListType::T2:
        t2_.removeNode(node);
        break;
      case ArcListType::B2:
        b2_.removeNode(node);
        break;
      }
      cacheMap_.erase(it);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool contains(const Key &key) const override {
    std::lock_guard lock(mutex_);
    if (auto it = cacheMap_.find(key); it != cacheMap_.end()) {
      // 只有在 T1 或 T2 中才算真正存在可读数据
      return it->second->type == ArcListType::T1 ||
             it->second->type == ArcListType::T2;
    }
    return false;
  }
  // 真实有效缓存大小（不包含幽灵元数据）
  [[nodiscard]] std::size_t size() const override {
    std::lock_guard lock(mutex_);
    return t1_.size() + t2_.size();
  }
  [[nodiscard]] std::size_t capacity() const noexcept override {
    return capacity_;
  }
  [[nodiscard]] bool empty() const override {
    std::lock_guard lock(mutex_);
    return (t1_.size() + t2_.size()) == 0;
  }
  void clear() override {
    std::lock_guard lock(mutex_);
    cacheMap_.clear();
    t1_.clear();
    b1_.clear();
    t2_.clear();
    b2_.clear();
    p_ = 0;
  }

private:
  void replace(bool inB2 = false) noexcept {
    std::size_t t1Size = t1_.size();
    if (t1Size > 0 && ((t1Size > p_) || (inB2 && t1Size == p_))) {
      // 从 T1 淘汰尾部至 B1（释放 Value 内存，就地变身为幽灵节点）
      auto *victim = t1_.popTail();
      if (victim) {
        victim->value.reset(); // 零堆重新分配，就地释放内存！
        victim->type = ArcListType::B1;
        b1_.pushFront(victim);
      }
    } else {
      // 从 T2 淘汰尾部至 B2（释放 Value 内存，就地变身为幽灵节点）
      auto *victim = t2_.popTail();
      if (victim) {
        victim->value.reset(); // 零堆重新分配，就地释放内存！
        victim->type = ArcListType::B2;
        b2_.pushFront(victim);
      }
    }
  }
  std::size_t capacity_{0};
  std::size_t p_{0};

  mutable std::mutex mutex_;
  std::unordered_map<Key, NodePtr> cacheMap_;

  ListType t1_{};
  ListType b1_{};
  ListType t2_{};
  ListType b2_{};
};
} // namespace YsCache
