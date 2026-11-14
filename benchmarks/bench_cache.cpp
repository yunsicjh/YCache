#include "YsCache/ArcCache.h"
#include "YsCache/CacheBuilder.h"
#include "YsCache/LfuCache.h"
#include "YsCache/LruCache.h"
#include "YsCache/MetricsCacheDecorator.h"
#include "YsCache/TtlCacheDecorator.h"

#include <benchmark/benchmark.h>
#include <chrono>
#include <memory>
#include <string>

using namespace YsCache;

// ============================================================================
// 1. 单操作纳秒级延迟评测 (Micro-latency: Hit & Eviction)
// ============================================================================

static void BM_Lru_Get_Hit(benchmark::State &state) {
  constexpr std::size_t kCapacity = 10000;
  LruCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache.get(key++ % kCapacity);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lru_Get_Hit);

static void BM_Lfu_Get_Hit(benchmark::State &state) {
  constexpr std::size_t kCapacity = 10000;
  LfuCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache.get(key++ % kCapacity);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lfu_Get_Hit);

static void BM_Arc_Get_Hit(benchmark::State &state) {
  constexpr std::size_t kCapacity = 10000;
  ArcCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache.get(key++ % kCapacity);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arc_Get_Hit);

static void BM_LruK_Get_Hit(benchmark::State &state) {
  constexpr std::size_t kCapacity = 10000;
  LruKCache<int, int> cache(kCapacity, kCapacity, 2);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache.get(key++ % kCapacity);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LruK_Get_Hit);

// 缓存满状态下的连续写入与淘汰
static void BM_Lru_Put_Evict(benchmark::State &state) {
  constexpr std::size_t kCapacity = 5000;
  LruCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = static_cast<int>(kCapacity);
  for (auto _ : state) {
    cache.put(key++, 12345);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lru_Put_Evict);

static void BM_Lfu_Put_Evict(benchmark::State &state) {
  constexpr std::size_t kCapacity = 5000;
  LfuCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = static_cast<int>(kCapacity);
  for (auto _ : state) {
    cache.put(key++, 12345);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lfu_Put_Evict);

static void BM_Arc_Put_Evict(benchmark::State &state) {
  constexpr std::size_t kCapacity = 5000;
  ArcCache<int, int> cache(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    cache.put(static_cast<int>(i), static_cast<int>(i));
  }
  int key = static_cast<int>(kCapacity);
  for (auto _ : state) {
    cache.put(key++, 12345);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arc_Put_Evict);

// ============================================================================
// 2. 装饰器模式损耗拆解 (Decorator Overhead Breakdown)
// ============================================================================

static void BM_Decorator_Baseline_RawLru(benchmark::State &state) {
  LruCache<int, int> cache(1000);
  for (int i = 0; i < 1000; ++i) {
    cache.put(i, i);
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache.get(key++ % 1000);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Decorator_Baseline_RawLru);

static void BM_Decorator_WithMetrics(benchmark::State &state) {
  auto cache = CacheBuilder<int, int>()
                   .withCapacity(1000)
                   .withPolicy(CachePolicyType::LRU)
                   .withMetrics(true)
                   .build();
  for (int i = 0; i < 1000; ++i) {
    cache->put(i, i);
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache->get(key++ % 1000);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Decorator_WithMetrics);

static void BM_Decorator_WithTtl(benchmark::State &state) {
  auto cache = CacheBuilder<int, int>()
                   .withCapacity(1000)
                   .withPolicy(CachePolicyType::LRU)
                   .withTtl(std::chrono::hours(1))
                   .build();
  for (int i = 0; i < 1000; ++i) {
    cache->put(i, i);
  }
  int key = 0;
  for (auto _ : state) {
    auto val = cache->get(key++ % 1000);
    benchmark::DoNotOptimize(val);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Decorator_WithTtl);

// ============================================================================
// 3. 多核高并发伪共享消抖对比 (Global Lock vs Sharded alignas(64))
// ============================================================================

static void BM_Concurrent_GlobalLock_Lru(benchmark::State &state) {
  static std::unique_ptr<LruCache<int, int>> cache;
  if (state.thread_index() == 0) {
    cache = std::make_unique<LruCache<int, int>>(10000);
    for (int i = 0; i < 5000; ++i) {
      cache->put(i, i);
    }
  }

  int thread_offset = static_cast<int>(state.thread_index() * 100000);
  int i = 0;
  for (auto _ : state) {
    int key = (i++ % 5000);
    if ((i & 7) == 0) {
      // 12.5% 写入
      cache->put(thread_offset + (i % 1000), i);
    } else {
      // 87.5% 读取
      auto val = cache->get(key);
      benchmark::DoNotOptimize(val);
    }
  }

  if (state.thread_index() == 0) {
    cache.reset();
  }
}
BENCHMARK(BM_Concurrent_GlobalLock_Lru)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

static void BM_Concurrent_Sharded_Lru(benchmark::State &state) {
  static std::unique_ptr<ShardedLruCache<int, int>> cache;
  if (state.thread_index() == 0) {
    cache = std::make_unique<ShardedLruCache<int, int>>(10000, 8);
    for (int i = 0; i < 5000; ++i) {
      cache->put(i, i);
    }
  }

  int thread_offset = static_cast<int>(state.thread_index() * 100000);
  int i = 0;
  for (auto _ : state) {
    int key = (i++ % 5000);
    if ((i & 7) == 0) {
      // 12.5% 写入
      cache->put(thread_offset + (i % 1000), i);
    } else {
      // 87.5% 读取
      auto val = cache->get(key);
      benchmark::DoNotOptimize(val);
    }
  }

  if (state.thread_index() == 0) {
    cache.reset();
  }
}
BENCHMARK(BM_Concurrent_Sharded_Lru)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK_MAIN();
