#include "YsCache/LruCache.h"
#include "gtest/gtest.h"
#include <string>
#include <thread>
#include <vector>

using namespace YsCache;

// 1. 基础读写测试
TEST(LruCacheTest, BasicPutAndGet) {
  LruCache<int, int> cache(2);

  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(cache.size(), 0);
  EXPECT_EQ(cache.capacity(), 2);

  cache.put(1, 100);
  EXPECT_FALSE(cache.empty());
  EXPECT_EQ(cache.size(), 1);

  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 100);

  // 查询不存在的键
  EXPECT_EQ(cache.get(999), std::nullopt);
}

// 2. 更新已有键的值
TEST(LruCacheTest, UpdateExistingKey) {
  LruCache<int, std::string> cache(2);

  cache.put(1, "hello");
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get(1), "hello");

  // 更新同一个 key
  cache.put(1, "world");
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get(1), "world");
}

// 3. LRU 淘汰逻辑测试
TEST(LruCacheTest, EvictionPolicy) {
  LruCache<int, int> cache(3);

  cache.put(1, 10);
  cache.put(2, 20);
  cache.put(3, 30);
  EXPECT_EQ(cache.size(), 3);

  // 访问 key 1，使其变为最近使用，此时状态从旧到新为: 2 -> 3 -> 1
  EXPECT_EQ(cache.get(1), 10);

  // 插入 key 4，触发淘汰，最久未使用的 key 2 应该被淘汰
  cache.put(4, 40);
  EXPECT_EQ(cache.size(), 3);
  EXPECT_EQ(cache.get(2), std::nullopt);
  EXPECT_EQ(cache.get(1), 10);
  EXPECT_EQ(cache.get(3), 30);
  EXPECT_EQ(cache.get(4), 40);

  // 更新 key 3，使其变为最近使用，此时状态从旧到新为: 1 -> 4 -> 3
  cache.put(3, 300);

  // 插入 key 5，此时最旧的是 key 1，应淘汰 key 1
  cache.put(5, 50);
  EXPECT_EQ(cache.get(1), std::nullopt);
  EXPECT_EQ(cache.get(3), 300);
  EXPECT_EQ(cache.get(4), 40);
  EXPECT_EQ(cache.get(5), 50);
}

// 4. 删除节点测试
TEST(LruCacheTest, RemoveOperation) {
  LruCache<int, int> cache(3);

  cache.put(1, 10);
  cache.put(2, 20);

  // 删除存在的 key
  EXPECT_TRUE(cache.remove(1));
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get(1), std::nullopt);

  // 重复删除已删除的 key
  EXPECT_FALSE(cache.remove(1));

  // 删除不存在的 key
  EXPECT_FALSE(cache.remove(100));

  // 重新插入被删除的 key
  cache.put(1, 100);
  EXPECT_EQ(cache.size(), 2);
  EXPECT_EQ(cache.get(1), 100);
}

// 5. 边界情况：容量为 0
TEST(LruCacheTest, ZeroCapacity) {
  LruCache<int, int> cache(0);

  EXPECT_EQ(cache.capacity(), 0);
  EXPECT_EQ(cache.size(), 0);
  EXPECT_TRUE(cache.empty());

  cache.put(1, 10);
  EXPECT_EQ(cache.size(), 0);
  EXPECT_EQ(cache.get(1), std::nullopt);
  EXPECT_FALSE(cache.remove(1));
}

// 6. 边界情况：容量为 1
TEST(LruCacheTest, SingleCapacity) {
  LruCache<std::string, int> cache(1);

  cache.put("a", 1);
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get("a"), 1);

  // 插入新元素应立刻淘汰旧元素
  cache.put("b", 2);
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get("a"), std::nullopt);
  EXPECT_EQ(cache.get("b"), 2);

  // 原地更新
  cache.put("b", 20);
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get("b"), 20);

  EXPECT_TRUE(cache.remove("b"));
  EXPECT_EQ(cache.size(), 0);
}

// 7. 清空缓存测试
TEST(LruCacheTest, Clear) {
  LruCache<int, int> cache(3);

  cache.put(1, 10);
  cache.put(2, 20);
  cache.put(3, 30);
  EXPECT_EQ(cache.size(), 3);

  cache.clear();
  EXPECT_EQ(cache.size(), 0);
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(cache.get(1), std::nullopt);
  EXPECT_EQ(cache.get(2), std::nullopt);
  EXPECT_EQ(cache.get(3), std::nullopt);

  // 清空后再次插入正常工作
  cache.put(4, 40);
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.get(4), 40);
}

// 8. 复杂对象与字符串类型测试
TEST(LruCacheTest, StringKeyAndValue) {
  LruCache<std::string, std::string> cache(2);

  cache.put("user:1001", "Alice");
  cache.put("user:1002", "Bob");

  EXPECT_EQ(cache.get("user:1001"), "Alice");
  cache.put("user:1003", "Charlie"); // 应淘汰 user:1002

  EXPECT_EQ(cache.get("user:1002"), std::nullopt);
  EXPECT_EQ(cache.get("user:1001"), "Alice");
  EXPECT_EQ(cache.get("user:1003"), "Charlie");
}

// 9. 多线程并发读写与淘汰测试
TEST(LruCacheTest, ConcurrentOperations) {
  constexpr int kCapacity = 50;
  constexpr int kThreadCount = 8;
  constexpr int kOpsPerThread = 1000;

  LruCache<int, int> cache(kCapacity);
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key = (t * 100) + (i % 30);
        cache.put(key, i);
        (void)cache.get(key);
        if (i % 7 == 0) {
          (void)cache.remove(key);
        }
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  // 校验缓存大小不超过容量上限
  EXPECT_LE(cache.size(), static_cast<std::size_t>(kCapacity));
}

// 10. 观察者模式：淘汰监听与异常隔离测试
TEST(LruCacheTest, EvictionCallback) {
  LruCache<int, std::string> cache(2);

  std::vector<std::pair<int, std::string>> evictedItems;
  cache.setEvictionCallback(
      [&evictedItems](const int &k, const std::string &v) {
        evictedItems.emplace_back(k, v);
      });

  cache.put(1, "one");
  cache.put(2, "two");
  EXPECT_TRUE(evictedItems.empty()); // 未发生淘汰

  // 插入第 3 个元素，应淘汰最旧的 key 1
  cache.put(3, "three");
  ASSERT_EQ(evictedItems.size(), 1);
  EXPECT_EQ(evictedItems[0].first, 1);
  EXPECT_EQ(evictedItems[0].second, "one");

  // 更新已有 key 不应触发淘汰
  cache.put(3, "three_updated");
  EXPECT_EQ(evictedItems.size(), 1);

  // 显式主动 remove 不应触发淘汰回调（主动删除 != 被动淘汰）
  EXPECT_TRUE(cache.remove(2));
  EXPECT_EQ(evictedItems.size(), 1);

  // 测试回调抛出异常时的韧性（Strong Exception Resilience）
  cache.setEvictionCallback([](const int &, const std::string &) {
    throw std::runtime_error("User callback failed intentionally");
  });

  cache.put(4, "four");
  cache.put(5, "five"); // 触发淘汰，且用户回调抛出异常

  // 缓存内部状态不损坏，依然能正常工作
  EXPECT_EQ(cache.size(), 2);
  EXPECT_EQ(cache.get(5), "five");
}
