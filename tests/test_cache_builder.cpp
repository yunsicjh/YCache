#include "YsCache/CacheBuilder.h"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using namespace YsCache;

// 1. 测试基础构建：四种核心淘汰策略
TEST(CacheBuilderTest, BuildBasicPolicies) {
  // LRU
  auto lru = CacheBuilder<int, std::string>()
                 .withCapacity(2)
                 .withPolicy(CachePolicyType::LRU)
                 .build();
  ASSERT_NE(lru, nullptr);
  lru->put(1, "one");
  lru->put(2, "two");
  EXPECT_EQ(lru->get(1), "one");

  // LFU
  auto lfu = CacheBuilder<int, std::string>()
                 .withCapacity(2)
                 .withPolicy(CachePolicyType::LFU)
                 .withMaxAvgFreq(20)
                 .build();
  ASSERT_NE(lfu, nullptr);
  lfu->put(1, "one");
  lfu->put(2, "two");
  EXPECT_EQ(lfu->get(1), "one");

  // ARC
  auto arc = CacheBuilder<int, std::string>()
                 .withCapacity(2)
                 .withPolicy(CachePolicyType::ARC)
                 .build();
  ASSERT_NE(arc, nullptr);
  arc->put(1, "one");
  arc->put(2, "two");
  EXPECT_EQ(arc->get(1), "one");

  // LRU-K
  auto lruk = CacheBuilder<int, std::string>()
                  .withCapacity(2)
                  .withPolicy(CachePolicyType::LRU_K)
                  .withK(2)
                  .withHistoryCapacity(4)
                  .build();
  ASSERT_NE(lruk, nullptr);
  lruk->put(1, "one");
  lruk->put(2, "two");
  EXPECT_EQ(lruk->get(1), "one");
}

// 2. 测试分片缓存构建
TEST(CacheBuilderTest, BuildShardedCaches) {
  auto shardedLru = CacheBuilder<std::string, int>()
                        .withCapacity(100)
                        .withPolicy(CachePolicyType::LRU)
                        .withShards(4)
                        .build();
  ASSERT_NE(shardedLru, nullptr);
  shardedLru->put("key1", 100);
  EXPECT_EQ(shardedLru->get("key1"), 100);

  auto shardedLfu = CacheBuilder<std::string, int>()
                        .withCapacity(100)
                        .withPolicy(CachePolicyType::LFU)
                        .withShards(4)
                        .build();
  ASSERT_NE(shardedLfu, nullptr);
  shardedLfu->put("key2", 200);
  EXPECT_EQ(shardedLfu->get("key2"), 200);
}

// 3. 测试 TTL 装饰器组装与过期机制
TEST(CacheBuilderTest, BuildWithTtl) {
  auto ttlCache = CacheBuilder<int, std::string>()
                      .withCapacity(10)
                      .withPolicy(CachePolicyType::LRU)
                      .withTtl(std::chrono::milliseconds(50))
                      .build();
  ASSERT_NE(ttlCache, nullptr);

  ttlCache->put(1, "expire_soon");
  EXPECT_EQ(ttlCache->get(1), "expire_soon");

  // 等待过期
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  EXPECT_EQ(ttlCache->get(1), std::nullopt);
}

// 4. 测试 Metrics 装饰器组装
TEST(CacheBuilderTest, BuildWithMetrics) {
  auto cache = CacheBuilder<int, int>()
                   .withCapacity(10)
                   .withPolicy(CachePolicyType::LRU)
                   .withMetrics(true)
                   .build();
  ASSERT_NE(cache, nullptr);

  cache->put(1, 100);
  EXPECT_EQ(cache->get(1), 100);  // 命中
  EXPECT_EQ(cache->get(2), std::nullopt); // 未命中

  // 通过 dynamic_cast 访问 MetricsCacheDecorator 的特定运维接口
  auto *metrics = dynamic_cast<MetricsCacheDecorator<int, int> *>(cache.get());
  ASSERT_NE(metrics, nullptr);
  EXPECT_EQ(metrics->hits(), 1);
  EXPECT_EQ(metrics->misses(), 1);
  EXPECT_EQ(metrics->totalRequests(), 2);
  EXPECT_DOUBLE_EQ(metrics->hitRate(), 50.0);
}

// 5. 测试淘汰监听器组装（Observer）
TEST(CacheBuilderTest, BuildWithEvictionCallback) {
  std::vector<std::pair<int, std::string>> evicted;
  auto cache = CacheBuilder<int, std::string>()
                   .withCapacity(2)
                   .withPolicy(CachePolicyType::LRU)
                   .withEvictionCallback([&evicted](const int &k, const std::string &v) {
                     evicted.emplace_back(k, v);
                   })
                   .build();
  ASSERT_NE(cache, nullptr);

  cache->put(1, "one");
  cache->put(2, "two");
  EXPECT_TRUE(evicted.empty());

  cache->put(3, "three"); // 触发淘汰 key 1
  ASSERT_EQ(evicted.size(), 1);
  EXPECT_EQ(evicted[0].first, 1);
  EXPECT_EQ(evicted[0].second, "one");
}

// 6. 全栈组合测试：分片 + TTL + Metrics + 观察者
TEST(CacheBuilderTest, FullStackAssembly) {
  std::vector<std::pair<std::string, std::string>> evicted;
  auto cache = CacheBuilder<std::string, std::string>()
                   .withCapacity(4)
                   .withPolicy(CachePolicyType::LRU)
                   .withShards(2)
                   .withTtl(std::chrono::milliseconds(100))
                   .withMetrics(true)
                   .withEvictionCallback([&evicted](const auto &k, const auto &v) {
                     evicted.emplace_back(k, v);
                   })
                   .build();
  ASSERT_NE(cache, nullptr);

  cache->put("k1", "v1");
  cache->put("k2", "v2");
  EXPECT_EQ(cache->get("k1"), "v1");

  auto *metrics = dynamic_cast<MetricsCacheDecorator<std::string, std::string> *>(cache.get());
  ASSERT_NE(metrics, nullptr);
  EXPECT_EQ(metrics->hits(), 1);
}

// 7. 异常防御性边界测试
TEST(CacheBuilderTest, InvalidArguments) {
  // 容量为 0 抛出异常
  EXPECT_THROW(
      ((void)CacheBuilder<int, int>().withCapacity(0).build()),
      std::invalid_argument);

  // 对不支持分片的策略设置分片抛出异常
  EXPECT_THROW(
      ((void)CacheBuilder<int, int>()
          .withCapacity(10)
          .withPolicy(CachePolicyType::ARC)
          .withShards(4)
          .build()),
      std::invalid_argument);
}
