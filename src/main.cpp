#include "YsCache/ArcCache.h"
#include "YsCache/LfuCache.h"
#include "YsCache/LruCache.h"
#include "YsCache/YsCachePolicy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

// 高精度计时器 (RAII / 现代 C++ 规范)
class Timer {
public:
  Timer() : start_(std::chrono::high_resolution_clock::now()) {}

  [[nodiscard]] double elapsed() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_).count();
  }

private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// 辅助函数：打印测试对比结果
void printResults(const std::string &testName, std::size_t capacity,
                  const std::vector<std::string> &names,
                  const std::vector<std::size_t> &get_operations,
                  const std::vector<std::size_t> &hits,
                  const std::vector<double> &durations = {}) {
  std::cout << "=== " << testName << " 结果汇总 ===" << std::endl;
  std::cout << "缓存容量: " << capacity << std::endl;

  for (std::size_t i = 0; i < hits.size(); ++i) {
    double hitRate =
        (get_operations[i] > 0)
            ? (100.0 * static_cast<double>(hits[i]) /
               static_cast<double>(get_operations[i]))
            : 0.0;
    std::string name =
        (i < names.size() ? names[i] : "Algorithm " + std::to_string(i + 1));
    std::cout << std::left << std::setw(12) << name
              << " - 命中率: " << std::fixed << std::setprecision(2)
              << std::setw(6) << hitRate << "% "
              << "(" << hits[i] << "/" << get_operations[i] << ")";
    if (i < durations.size()) {
      std::cout << " | 耗时: " << std::fixed << std::setprecision(1)
                << durations[i] << " ms";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

// 测试场景1：热点数据访问测试
void testHotDataAccess() {
  std::cout << "\n=== 测试场景1：热点数据访问测试 ===" << std::endl;

  constexpr std::size_t CAPACITY = 20;       // 缓存容量
  constexpr std::size_t OPERATIONS = 500000; // 总操作次数
  constexpr std::size_t HOT_KEYS = 20;       // 热点数据数量
  constexpr std::size_t COLD_KEYS = 5000;    // 冷数据数量

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  // LRU-K 参数说明：
  // - 主缓存容量与其他算法相同
  // - 历史记录容量设为可能访问的所有键数量
  // - k=2 表示数据被访问 2 次后才会晋升进入主缓存，用于过滤偶发性冷数据
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, HOT_KEYS + COLD_KEYS, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 20000);

  const std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<std::size_t> hits(caches.size(), 0);
  std::vector<std::size_t> get_operations(caches.size(), 0);
  std::vector<double> durations(caches.size(), 0.0);

  constexpr unsigned int SEED = 42;
  std::mt19937 gen;

  // 为每种缓存算法进行完全相同的操作序列测试
  for (std::size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED); // 重置随机种子，确保所有算法面临绝对一致的请求序列
    Timer timer;

    // 先预热缓存，插入初始数据（采用 C++17 Sink Argument 完美转移）
    for (std::size_t key = 0; key < HOT_KEYS; ++key) {
      caches[i]->put(static_cast<int>(key), "value" + std::to_string(key));
    }

    // 交替进行 put 和 get 操作，模拟真实场景（30% 写，70% 读）
    for (std::size_t op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 30);
      int key;

      // 70% 概率访问热点数据，30% 概率访问冷数据
      if (gen() % 100 < 70) {
        key = static_cast<int>(gen() % HOT_KEYS);
      } else {
        key = static_cast<int>(HOT_KEYS + (gen() % COLD_KEYS));
      }

      if (isPut) {
        caches[i]->put(key, "value" + std::to_string(key) + "_v" +
                                std::to_string(op % 100));
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("热点数据访问测试", CAPACITY, names, get_operations, hits, durations);
}

// 测试场景2：循环扫描测试
void testLoopPattern() {
  std::cout << "\n=== 测试场景2：循环扫描测试 ===" << std::endl;

  constexpr std::size_t CAPACITY = 50;       // 缓存容量
  constexpr std::size_t LOOP_SIZE = 500;     // 循环范围大小
  constexpr std::size_t OPERATIONS = 200000; // 总操作次数

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  // LRU-K 参数：历史记录容量设为循环大小的 2 倍，k=2 抵御循环污染
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, LOOP_SIZE * 2, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 3000);

  const std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<std::size_t> hits(caches.size(), 0);
  std::vector<std::size_t> get_operations(caches.size(), 0);
  std::vector<double> durations(caches.size(), 0.0);

  constexpr unsigned int SEED = 42;
  std::mt19937 gen;

  for (std::size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED);
    Timer timer;

    // 预热部分数据（只加载 20% 的数据）
    for (std::size_t key = 0; key < LOOP_SIZE / 5; ++key) {
      caches[i]->put(static_cast<int>(key), "loop" + std::to_string(key));
    }

    int current_pos = 0;

    // 20% 写操作，80% 读操作
    for (std::size_t op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 20);
      int key;

      if (op % 100 < 60) { // 60% 顺序扫描
        key = current_pos;
        current_pos = static_cast<int>((current_pos + 1) % LOOP_SIZE);
      } else if (op % 100 < 90) { // 30% 随机跳跃
        key = static_cast<int>(gen() % LOOP_SIZE);
      } else { // 10% 访问范围外数据
        key = static_cast<int>(LOOP_SIZE + (gen() % LOOP_SIZE));
      }

      if (isPut) {
        caches[i]->put(key, "loop" + std::to_string(key) + "_v" +
                                std::to_string(op % 100));
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("循环扫描测试", CAPACITY, names, get_operations, hits, durations);
}

// 测试场景3：工作负载剧烈变化测试
void testWorkloadShift() {
  std::cout << "\n=== 测试场景3：工作负载剧烈变化测试 ===" << std::endl;

  constexpr std::size_t CAPACITY = 30;                 // 缓存容量
  constexpr std::size_t OPERATIONS = 80000;            // 总操作次数
  constexpr std::size_t PHASE_LENGTH = OPERATIONS / 5; // 每个阶段的长度（16000 次操作）

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, 500, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 10000);

  const std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<std::size_t> hits(caches.size(), 0);
  std::vector<std::size_t> get_operations(caches.size(), 0);
  std::vector<double> durations(caches.size(), 0.0);

  constexpr unsigned int SEED = 42;
  std::mt19937 gen;

  for (std::size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED);
    Timer timer;

    // 预热缓存，插入少量初始数据
    for (int key = 0; key < 30; ++key) {
      caches[i]->put(key, "init" + std::to_string(key));
    }

    // 进行多阶段动态测试
    for (std::size_t op = 0; op < OPERATIONS; ++op) {
      std::size_t phase = op / PHASE_LENGTH;

      unsigned int putProbability;
      switch (phase) {
      case 0:
        putProbability = 15;
        break; // 阶段1: 热点访问，15% 写入
      case 1:
        putProbability = 30;
        break; // 阶段2: 大范围随机，30% 写入
      case 2:
        putProbability = 10;
        break; // 阶段3: 顺序扫描，10% 写入
      case 3:
        putProbability = 25;
        break; // 阶段4: 局部性随机，25% 写入
      case 4:
        putProbability = 20;
        break; // 阶段5: 混合访问，20% 写入
      default:
        putProbability = 20;
      }

      bool isPut = (gen() % 100 < putProbability);
      int key;

      if (op < PHASE_LENGTH) { // 阶段1: 热点访问 (5 个热点键)
        key = static_cast<int>(gen() % 5);
      } else if (op < PHASE_LENGTH * 2) { // 阶段2: 大范围随机 (400 个键)
        key = static_cast<int>(gen() % 400);
      } else if (op < PHASE_LENGTH * 3) { // 阶段3: 顺序扫描 (100 个键)
        key = static_cast<int>((op - PHASE_LENGTH * 2) % 100);
      } else if (op < PHASE_LENGTH * 4) { // 阶段4: 局部性随机 (5 个局部区域)
        std::size_t locality = (op / 800) % 5;
        key = static_cast<int>(locality * 15 + (gen() % 15));
      } else { // 阶段5: 混合访问
        int r = gen() % 100;
        if (r < 40) {
          key = static_cast<int>(gen() % 5); // 40% 热点
        } else if (r < 70) {
          key = static_cast<int>(5 + (gen() % 45)); // 30% 中等范围
        } else {
          key = static_cast<int>(50 + (gen() % 350)); // 30% 大范围
        }
      }

      if (isPut) {
        caches[i]->put(key, "value" + std::to_string(key) + "_p" +
                                std::to_string(phase));
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("工作负载剧烈变化测试", CAPACITY, names, get_operations, hits, durations);
}

// 测试场景4：多线程高并发吞吐与硬件伪共享隔离基准测试（战役2核心成果验证）
void testConcurrentThroughput() {
  std::cout << "\n=== 测试场景4：多线程高并发吞吐与硬件防伪共享测试 (战役2 核心成果) ==="
            << std::endl;

  const unsigned int hardwareThreads =
      std::max(2u, std::thread::hardware_concurrency());
  const std::size_t THREAD_COUNT = hardwareThreads;
  constexpr std::size_t OPS_PER_THREAD = 100000;
  constexpr std::size_t CAPACITY = 1000;

  std::cout << "测试环境: " << THREAD_COUNT << " 个工作线程并发, 每线程 "
            << OPS_PER_THREAD << " 次混合读写 (总计 "
            << (THREAD_COUNT * OPS_PER_THREAD) << " 次操作)" << std::endl;
  std::cout << "缓存容量: " << CAPACITY << std::endl;

  struct BenchmarkTarget {
    std::string name;
    std::unique_ptr<YsCache::YsCachePolicy<int, std::string>> cache;
  };

  std::vector<BenchmarkTarget> targets;
  targets.push_back(
      {"标准 LRU (单全局锁)",
       std::make_unique<YsCache::LruCache<int, std::string>>(CAPACITY)});
  targets.push_back(
      {"分片 LRU (alignas(64) 隔离)",
       std::make_unique<YsCache::ShardedLruCache<int, std::string>>(
           CAPACITY, THREAD_COUNT)});
  targets.push_back(
      {"分片 LFU (alignas(64) 隔离)",
       std::make_unique<YsCache::ShardedLfuCache<int, std::string>>(
           CAPACITY, THREAD_COUNT)});

  double baselineDuration = 0.0;

  for (std::size_t idx = 0; idx < targets.size(); ++idx) {
    auto &target = targets[idx];
    auto *cache = target.cache.get();

    // 预热部分数据
    for (int key = 0; key < 100; ++key) {
      cache->put(key, "init_" + std::to_string(key));
    }

    std::vector<std::thread> workers;
    workers.reserve(THREAD_COUNT);

    Timer timer;
    for (std::size_t t = 0; t < THREAD_COUNT; ++t) {
      workers.emplace_back([cache, t]() {
        std::mt19937 threadGen(static_cast<unsigned int>(42 + t));
        for (std::size_t op = 0; op < OPS_PER_THREAD; ++op) {
          int key = static_cast<int>((t * 200) + (threadGen() % 300));
          if (op % 10 < 3) {
            // 30% 写
            cache->put(key, "data_" + std::to_string(op));
          } else {
            // 70% 读
            (void)cache->get(key);
          }
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }
    double elapsedMs = timer.elapsed();

    if (idx == 0) {
      baselineDuration = elapsedMs;
    }

    double totalOpsAll = static_cast<double>(THREAD_COUNT * OPS_PER_THREAD);
    double qps = (totalOpsAll / (elapsedMs / 1000.0));
    double speedup = (elapsedMs > 0.0) ? (baselineDuration / elapsedMs) : 1.0;

    std::cout << std::left << std::setw(30) << target.name
              << " | 耗时: " << std::fixed << std::setprecision(1)
              << std::setw(6) << elapsedMs << " ms"
              << " | 吞吐: " << std::fixed << std::setprecision(0)
              << std::setw(9) << qps << " Ops/s";
    if (idx > 0) {
      std::cout << " | 加速比: " << std::fixed << std::setprecision(2)
                << speedup << "x";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

int main() {
  testHotDataAccess();
  testLoopPattern();
  testWorkloadShift();
  testConcurrentThroughput();
  return 0;
}
