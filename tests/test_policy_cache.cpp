#include "YsCache/PolicyCache.h"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using namespace YsCache;

// 1. FastLruCache 测试（LRU + 无锁单线程）
TEST(PolicyCacheTest, FastLruBasicOperations) {
  FastLruCache<int, std::string> cache(2);

  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(cache.size(), 0);
  EXPECT_EQ(cache.capacity(), 2);

  cache.put(1, "one");
  cache.put(2, "two");
  EXPECT_EQ(cache.size(), 2);

  // 访问 1，热度更新，当前由旧到新为: 2 -> 1
  EXPECT_EQ(cache.get(1), "one");

  // 插入 3，应淘汰最久未访问的 2
  cache.put(3, "three");
  EXPECT_EQ(cache.size(), 2);
  EXPECT_EQ(cache.get(2), std::nullopt);
  EXPECT_EQ(cache.get(1), "one");
  EXPECT_EQ(cache.get(3), "three");

  // 测试 peek: 只读窥视不改变淘汰顺序
  EXPECT_EQ(cache.peek(1), "one");
}

// 2. FastFifoCache 测试（FIFO 先进先出，访问不改变顺序）
TEST(PolicyCacheTest, FastFifoOrder) {
  FastFifoCache<int, std::string> cache(2);

  cache.put(1, "one");
  cache.put(2, "two");

  // 访问 1。在 LRU 中 1 会被推到最新，但在 FIFO 中顺序不变（1 依然是最早进入的）
  EXPECT_EQ(cache.get(1), "one");

  // 插入 3，应淘汰最早进入的 1，而不是 2！
  cache.put(3, "three");
  EXPECT_EQ(cache.get(1), std::nullopt);
  EXPECT_EQ(cache.get(2), "two");
  EXPECT_EQ(cache.get(3), "three");
}

// 3. FastLfuCache 测试（LFU 频次优先）
TEST(PolicyCacheTest, FastLfuOrder) {
  FastLfuCache<int, std::string> cache(2);

  cache.put(1, "one");
  cache.put(2, "two");

  // 增加 1 的访问频次
  (void)cache.get(1);
  (void)cache.get(1);

  // 插入 3，key 2 的频次为 1，应淘汰 key 2
  cache.put(3, "three");
  EXPECT_EQ(cache.get(2), std::nullopt);
  EXPECT_EQ(cache.get(1), "one");
  EXPECT_EQ(cache.get(3), "three");
}

// 4. FastLruKCache 测试（LRU-K 访问 K 次晋升热队列）
TEST(PolicyCacheTest, FastLruKPromotion) {
  // capacity = 2, historyCapacity = 2, k = 2
  PolicyCache<int, std::string, LruKEvictionPolicy, SingleThreadPolicy> cache(
      2, 2, 2);

  cache.put(1, "one");
  cache.put(2, "two");

  // key 1 访问第 2 次，晋升到热队列
  (void)cache.get(1);

  // 插入 key 3，key 2 仍在冷队列且访问次数少，应淘汰 key 2
  cache.put(3, "three");
  EXPECT_EQ(cache.get(2), std::nullopt);
  EXPECT_EQ(cache.get(1), "one");
  EXPECT_EQ(cache.get(3), "three");
}

// 5. ConcurrentLruCache 多线程并发安全测试
TEST(PolicyCacheTest, MultiThreadedSafety) {
  ConcurrentLruCache<int, int> cache(100);
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 1000;

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&cache, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key = (t * 50) + (i % 50);
        cache.put(key, i);
        (void)cache.get(key);
        (void)cache.peek(key);
      }
    });
  }

  for (auto &w : workers) {
    w.join();
  }

  EXPECT_LE(cache.size(), 100);
}

// 6. 淘汰监听器测试
TEST(PolicyCacheTest, EvictionCallback) {
  std::vector<std::pair<int, std::string>> evicted;
  FastLruCache<int, std::string> cache(2);
  cache.setEvictionCallback([&evicted](const int &k, const std::string &v) {
    evicted.emplace_back(k, v);
  });

  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(3, "three"); // 触发淘汰 1

  ASSERT_EQ(evicted.size(), 1);
  EXPECT_EQ(evicted[0].first, 1);
  EXPECT_EQ(evicted[0].second, "one");
}

// 7. 异常防御边界
TEST(PolicyCacheTest, InvalidCapacityThrows) {
  EXPECT_THROW(
      ((void)FastLruCache<int, int>(0)),
      std::invalid_argument);
  EXPECT_THROW(
      ((void)FastArcCache<int, int>(0)),
      std::invalid_argument);
}

// 8. FastArcCache 自适应热度与幽灵队列测试
TEST(PolicyCacheTest, FastArcAdaptiveBehavior) {
  FastArcCache<int, std::string> cache(2);

  EXPECT_EQ(cache.targetT1Size(), 0);
  cache.put(1, "one");
  cache.put(2, "two");

  // Peek 不改变状态
  EXPECT_EQ(cache.peek(1), "one");

  // Get 访问 1，提拔到 T2
  EXPECT_EQ(cache.get(1), "one");

  // 插入 3，因 p=0 且 T1 存在元素 2，元素 2 被驱逐至 B1（幽灵队列）
  cache.put(3, "three");
  EXPECT_EQ(cache.size(), 2);
  EXPECT_EQ(cache.get(2), std::nullopt); // B1 幽灵不返回实体
  EXPECT_EQ(cache.get(1), "one");        // T2 仍保留
  EXPECT_EQ(cache.get(3), "three");      // T1 中

  // 幽灵命中（Ghost Hit）：再次写入 2
  // 命中 B1，自适应学习参数 p 增加，偏向近期访问
  cache.put(2, "two_v2");
  EXPECT_GT(cache.targetT1Size(), 0);
  EXPECT_EQ(cache.get(2), "two_v2");
}

// 9. FastArcCache 淘汰回调与幽灵大小测试
TEST(PolicyCacheTest, FastArcEvictionCallback) {
  std::vector<std::pair<int, std::string>> evicted;
  FastArcCache<int, std::string> cache(2, [&evicted](const int &k, const std::string &v) {
    evicted.emplace_back(k, v);
  });

  cache.put(1, "one");
  cache.put(2, "two");
  // 先将 1 提升至 T2，此时 T1={2}, T2={1}
  (void)cache.get(1);

  // 插入 3 触发 replace(false)，导致 2 被淘汰至 B1 幽灵队列（Value 释放并触发回调）
  cache.put(3, "three");

  ASSERT_EQ(evicted.size(), 1);
  EXPECT_EQ(evicted[0].first, 2);
  EXPECT_EQ(evicted[0].second, "two");
  EXPECT_EQ(cache.ghostSize(), 1);
}

// 10. ConcurrentArcCache 多线程并发安全测试
TEST(PolicyCacheTest, ConcurrentArcMultiThreadedSafety) {
  ConcurrentArcCache<int, int> cache(100);
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 1000;

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&cache, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key = (t * 50) + (i % 50);
        cache.put(key, i);
        (void)cache.get(key);
        (void)cache.peek(key);
      }
    });
  }

  for (auto &w : workers) {
    w.join();
  }

  EXPECT_LE(cache.size(), 100);
}

// 11. 编译期静态断言契约测试（Compile-Time Concepts Check）
struct IncompletePolicy {
  // 故意缺少 selectVictim 和 onAccess 等方法
  struct Node : public EvictionNodeBase {};
  using NodeType = Node;
  void onInsert(Node *) noexcept {}
};

struct IncompleteConcurrency {
  // 故意缺少 SharedLock
  using UniqueLock = int;
};

TEST(PolicyCacheTest, CompileTimeContractVerification) {
  // 正常策略必须满足契约
  static_assert(
      detail::is_valid_eviction_policy<LruEvictionPolicy<int, int>,
                                       LruEvictionPolicy<int, int>::NodeType>::value,
      "LruEvictionPolicy must satisfy contract");

  static_assert(
      detail::is_valid_concurrency_policy<SingleThreadPolicy>::value,
      "SingleThreadPolicy must satisfy contract");

  // 不合规的残缺策略必须被契约侦测器准确拦截并判为 false
  static_assert(
      !detail::is_valid_eviction_policy<IncompletePolicy,
                                        IncompletePolicy::NodeType>::value,
      "IncompletePolicy must be rejected");

  static_assert(
      !detail::is_valid_concurrency_policy<IncompleteConcurrency>::value,
      "IncompleteConcurrency must be rejected");
}

// 12. StaticMetricsDecorator 静态指标装饰器测试
TEST(PolicyCacheTest, StaticMetricsDecoratorOperations) {
  StaticMetricsDecorator<FastLruCache<int, std::string>> cache(2);

  EXPECT_EQ(cache.hitCount(), 0);
  EXPECT_EQ(cache.missCount(), 0);
  EXPECT_EQ(cache.putCount(), 0);
  EXPECT_DOUBLE_EQ(cache.hitRate(), 0.0);

  cache.put(1, "one");
  cache.put(2, "two");
  EXPECT_EQ(cache.putCount(), 2);

  // 命中 1 次，未命中 1 次
  EXPECT_EQ(cache.get(1), "one");
  EXPECT_EQ(cache.get(999), std::nullopt);

  EXPECT_EQ(cache.hitCount(), 1);
  EXPECT_EQ(cache.missCount(), 1);
  EXPECT_EQ(cache.totalRequests(), 2);
  EXPECT_DOUBLE_EQ(cache.hitRate(), 0.5);

  // 纯读窥视 peek 也统计命中
  EXPECT_EQ(cache.peek(2), "two");
  EXPECT_EQ(cache.hitCount(), 2);
  EXPECT_DOUBLE_EQ(cache.hitRate(), 2.0 / 3.0);

  // 删除统计
  EXPECT_TRUE(cache.remove(1));
  EXPECT_EQ(cache.removeCount(), 1);

  // 重置指标
  cache.resetMetrics();
  EXPECT_EQ(cache.hitCount(), 0);
  EXPECT_EQ(cache.missCount(), 0);
  EXPECT_EQ(cache.putCount(), 0);
  EXPECT_EQ(cache.removeCount(), 0);
}

// 13. StaticTtlDecorator 静态 TTL 装饰器测试
TEST(PolicyCacheTest, StaticTtlDecoratorOperations) {
  using namespace std::chrono_literals;
  StaticTtlDecorator<int, std::string, FastLruCache> cache(50ms, 2);

  cache.put(1, "one");
  cache.put(2, "two");

  // 立即读取：命中
  EXPECT_EQ(cache.get(1), "one");
  EXPECT_TRUE(cache.contains(2));

  // 休眠超过 50ms 使数据过期
  std::this_thread::sleep_for(60ms);

  // 再次读取：已惰性过期
  EXPECT_EQ(cache.get(1), std::nullopt);
  EXPECT_FALSE(cache.contains(2));
  EXPECT_EQ(cache.peek(2), std::nullopt);
}

// 14. 静态装饰器多层无缝嵌套组合测试（Metrics 包装 TTL 包装 FastArcCache）
TEST(PolicyCacheTest, StackedStaticDecoratorsWithFastArc) {
  using namespace std::chrono_literals;
  // 零虚表、零堆分配包装栈：Metrics -> TTL -> FastArcCache
  StaticMetricsDecorator<StaticTtlDecorator<int, std::string, FastArcCache>> cache(
      50ms, 10);

  cache.put(100, "apple");
  EXPECT_EQ(cache.putCount(), 1);

  // 立即访问命中
  EXPECT_EQ(cache.get(100), "apple");
  EXPECT_EQ(cache.hitCount(), 1);

  // 过期后访问导致未命中
  std::this_thread::sleep_for(60ms);
  EXPECT_EQ(cache.get(100), std::nullopt);
  EXPECT_EQ(cache.missCount(), 1);
  EXPECT_DOUBLE_EQ(cache.hitRate(), 0.5);
}

