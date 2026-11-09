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

template <typename Key, typename Value>
class ArcCache : public YsCachePolicy<Key, Value> {
private:
  // 核心节点模型
  struct ArcNode {
    std::optional<Key> key;
    std::optional<Value> value; // 处于 B1/B2 幽灵队列时置为 nullopt
    ArcListType type;
    std::weak_ptr<ArcNode> prev;
    std::shared_ptr<ArcNode> next;

    ArcNode() = default;
    ArcNode(Key k, Value v, ArcListType t)
        : key(std::move(k)), value(std::move(v)), type(t) {}
    ArcNode(Key k, ArcListType t) : key(std::move(k)), type(t) {}
  };

  using NodePtr = std::shared_ptr<ArcNode>;

  class ArcList {
  public:
    ArcList() { initializeList(); }

    ~ArcList() {
      // 迭代断开链表指针，防止深层递归析构导致栈溢出
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

    void pushFront(NodePtr node) {
      node->next = dummyHead_->next;
      dummyHead_->next = node;
      node->next->prev = node;
      node->prev = dummyHead_;
      size_++;
    }

    void removeNode(NodePtr node) {
      auto prevNode = node->prev.lock();
      if (prevNode && node->next) {
        prevNode->next = node->next;
        node->next->prev = prevNode;
        node->prev.reset();
        node->next = nullptr;
        size_--;
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

    bool empty() const { return dummyHead_->next == dummyTail_; }
    size_t size() const { return size_; }

  private:
    NodePtr dummyHead_;
    NodePtr dummyTail_;
    size_t size_{0};

    void initializeList() {
      dummyHead_ = std::make_shared<ArcNode>();
      dummyTail_ = std::make_shared<ArcNode>();
      dummyHead_->next = dummyTail_;
      dummyTail_->prev = dummyHead_;
    }
  };

public:
  explicit ArcCache(size_t capacity) : capacity_(capacity), p_(0) {
    t1_ = std::make_unique<ArcList>();
    b1_ = std::make_unique<ArcList>();
    t2_ = std::make_unique<ArcList>();
    b2_ = std::make_unique<ArcList>();
  }

  // 显式禁用拷贝与移动
  ArcCache(const ArcCache &) = delete;
  ArcCache &operator=(const ArcCache &) = delete;
  ArcCache(ArcCache &&) = delete;
  ArcCache &operator=(ArcCache &&) = delete;

  ~ArcCache() override { clear(); }

  // 核心对外接口
  void put(Key key, Value value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0) {
      return;
    }

    auto it = cacheMap_.find(key);
    if (it != cacheMap_.end()) {
      auto node = it->second;

      // 分支 1：命中真实数据区 (T1 或 T2)，更新值并提升至 T2
      if (node->type == ArcListType::T1 || node->type == ArcListType::T2) {
        if (node->type == ArcListType::T1) {
          t1_->removeNode(node);
        } else {
          t2_->removeNode(node);
        }
        node->value = std::move(value);
        node->type = ArcListType::T2;
        t2_->pushFront(node);
        return;
      }

      // 分支 2：命中幽灵链表 B1 (扩大最近访问配额 p)
      if (node->type == ArcListType::B1) {
        size_t delta =
            (b1_->size() >= b2_->size()) ? 1 : (b2_->size() / b1_->size());
        p_ = std::min(capacity_, p_ + std::max<size_t>(1, delta));

        replace(key);

        b1_->removeNode(node);
        node->value = std::move(value);
        node->type = ArcListType::T2;
        t2_->pushFront(node);
        return;
      }

      // 分支 3：命中幽灵链表 B2 (缩小 p，倾斜给高频区)
      if (node->type == ArcListType::B2) {
        size_t delta =
            (b2_->size() >= b1_->size()) ? 1 : (b1_->size() / b2_->size());
        size_t step = std::max<size_t>(1, delta);
        p_ = (p_ > step) ? (p_ - step) : 0;

        replace(key);

        b2_->removeNode(node);
        node->value = std::move(value);
        node->type = ArcListType::T2;
        t2_->pushFront(node);
        return;
      }
    }

    // 分支 4：完全未命中 (全新 Key)
    size_t totalL1 = t1_->size() + b1_->size();
    size_t totalAll = totalL1 + t2_->size() + b2_->size();

    // 1. 严格维持历史幽灵上限与真实存储空间 (遵循 ARC Case IV 规范)
    if (totalL1 >= capacity_) {
      if (t1_->size() < capacity_) {
        auto ghostNode = b1_->popTail();
        if (ghostNode && ghostNode->key) {
          cacheMap_.erase(ghostNode->key.value());
        }
        replace(key);
      } else {
        // T1 已经独占全部物理容量，直接淘汰 T1 尾部，不转入 B1
        auto victim = t1_->popTail();
        if (victim && victim->key) {
          cacheMap_.erase(victim->key.value());
        }
      }
    } else {
      if (totalAll >= 2 * capacity_) {
        auto ghostNode = b2_->popTail();
        if (ghostNode && ghostNode->key) {
          cacheMap_.erase(ghostNode->key.value());
        }
      }
      if (t1_->size() + t2_->size() >= capacity_) {
        replace(key);
      }
    }

    // 2. 构建全新节点并插入 T1
    auto newNode =
        std::make_shared<ArcNode>(key, std::move(value), ArcListType::T1);
    t1_->pushFront(newNode);
    cacheMap_[key] = newNode;
  }

  std::optional<Value> get(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cacheMap_.find(key);
    if (it == cacheMap_.end()) {
      return std::nullopt;
    }
    auto node = it->second;

    // 只有命中真实数据区 (T1 或 T2)，才返回数据并提升至 T2
    if (node->type == ArcListType::T1 || node->type == ArcListType::T2) {
      if (node->type == ArcListType::T1) {
        t1_->removeNode(node);
      } else {
        t2_->removeNode(node);
      }
      node->type = ArcListType::T2;
      t2_->pushFront(node);
      return node->value;
    }

    // 若命中 B1 或 B2，属于幽灵记录（无真实数据），静默返回 nullopt
    // 待上层回填调用 put 时，再在 put 中完成 p 的调优与节点复活
    return std::nullopt;
  }

  bool remove(const Key &key) override {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cacheMap_.find(key);
    if (it == cacheMap_.end()) {
      return false;
    }

    auto node = it->second;
    bool wasRealData =
        (node->type == ArcListType::T1 || node->type == ArcListType::T2);

    // 根据所属链表类型精准移除
    switch (node->type) {
    case ArcListType::T1:
      t1_->removeNode(node);
      break;
    case ArcListType::T2:
      t2_->removeNode(node);
      break;
    case ArcListType::B1:
      b1_->removeNode(node);
      break;
    case ArcListType::B2:
      b2_->removeNode(node);
      break;
    }

    // 从全局索引中抹除
    cacheMap_.erase(it);
    return wasRealData;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheMap_.clear();
    t1_ = std::make_unique<ArcList>();
    b1_ = std::make_unique<ArcList>();
    t2_ = std::make_unique<ArcList>();
    b2_ = std::make_unique<ArcList>();
    p_ = 0;
  }

  // 获取真实物理缓存数据量 (|T1| + |T2|)
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return t1_->size() + t2_->size();
  }

  size_t capacity() const noexcept { return capacity_; }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (t1_->size() + t2_->size()) == 0;
  }

private:
  void replace(const Key &key) {
    // 1. 判定 key 当前是否处于 B2 中
    auto it = cacheMap_.find(key);
    bool inB2 = (it != cacheMap_.end() && it->second->type == ArcListType::B2);

    // 2. 判定淘汰源：T1 非空，且满足 (T1 超过配额) 或 (刚好等于配额且 key 在 B2 中)
    if (!t1_->empty() && (t1_->size() > p_ || (inB2 && t1_->size() == p_))) {
      // 从 T1 尾部淘汰
      auto node = t1_->popTail();
      if (node) {
        // 清空 Value 释放内存，降级为 B1 幽灵节点
        node->value.reset();
        node->type = ArcListType::B1;
        b1_->pushFront(node);
      }
    } else {
      // 从 T2 尾部淘汰
      auto node = t2_->popTail();
      if (node) {
        // 清空 Value 释放内存，降级为 B2 幽灵节点
        node->value.reset();
        node->type = ArcListType::B2;
        b2_->pushFront(node);
      }
    }
  }

  size_t capacity_{0}; // 物理缓存总容量 c
  size_t p_{0};        // 目标配额边界 p

  // 四大链表
  std::unique_ptr<ArcList> t1_;
  std::unique_ptr<ArcList> b1_;
  std::unique_ptr<ArcList> t2_;
  std::unique_ptr<ArcList> b2_;

  std::unordered_map<Key, NodePtr> cacheMap_; // 全局索引
  mutable std::mutex mutex_;
};

} // namespace YsCache
