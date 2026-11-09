#include "YsCache/ArcCache.h"
#include "YsCache/LfuCache.h"
#include "YsCache/LruCache.h"
#include "YsCache/YsCachePolicy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// 高精度计时器
class Timer {
public:
  Timer() : start_(std::chrono::high_resolution_clock::now()) {}

  double elapsed() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_).count();
  }

private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// 辅助函数：打印测试对比结果
void printResults(const std::string &testName, int capacity,
                  const std::vector<int> &get_operations,
                  const std::vector<int> &hits,
                  const std::vector<double> &durations = {}) {
  std::cout << "=== " << testName << " 结果汇总 ===" << std::endl;
  std::cout << "缓存容量: " << capacity << std::endl;

  const std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};

  for (size_t i = 0; i < hits.size(); ++i) {
    double hitRate =
        (get_operations[i] > 0) ? (100.0 * hits[i] / get_operations[i]) : 0.0;
    std::string name =
        (i < names.size() ? names[i] : "Algorithm " + std::to_string(i + 1));
    std::cout << std::left << std::setw(10) << name
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

  const int CAPACITY = 20;       // 缓存容量
  const int OPERATIONS = 500000; // 总操作次数
  const int HOT_KEYS = 20;       // 热点数据数量
  const int COLD_KEYS = 5000;    // 冷数据数量

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  // LRU-K 参数说明：
  // - 主缓存容量与其他算法相同
  // - 历史记录容量设为可能访问的所有键数量
  // - k=2 表示数据被访问 2 次后才会晋升进入主缓存，用于过滤偶发性冷数据
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, HOT_KEYS + COLD_KEYS, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 20000);

  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<int> hits(5, 0);
  std::vector<int> get_operations(5, 0);
  std::vector<double> durations(5, 0.0);

  const unsigned int SEED = 42;
  std::mt19937 gen;

  // 为每种缓存算法进行完全相同的操作序列测试
  for (size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED); // 重置随机种子，确保所有算法面临绝对一致的请求序列
    Timer timer;

    // 先预热缓存，插入初始数据
    for (int key = 0; key < HOT_KEYS; ++key) {
      std::string value = "value" + std::to_string(key);
      caches[i]->put(key, value);
    }

    // 交替进行 put 和 get 操作，模拟真实场景（30% 写，70% 读）
    for (int op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 30);
      int key;

      // 70% 概率访问热点数据，30% 概率访问冷数据
      if (gen() % 100 < 70) {
        key = gen() % HOT_KEYS;
      } else {
        key = HOT_KEYS + (gen() % COLD_KEYS);
      }

      if (isPut) {
        std::string value =
            "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
        caches[i]->put(key, value);
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("热点数据访问测试", CAPACITY, get_operations, hits, durations);
}

// 测试场景2：循环扫描测试
void testLoopPattern() {
  std::cout << "\n=== 测试场景2：循环扫描测试 ===" << std::endl;

  const int CAPACITY = 50;       // 缓存容量
  const int LOOP_SIZE = 500;     // 循环范围大小
  const int OPERATIONS = 200000; // 总操作次数

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  // LRU-K 参数：历史记录容量设为循环大小的 2 倍，k=2 抵御循环污染
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, LOOP_SIZE * 2, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 3000);

  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<int> hits(5, 0);
  std::vector<int> get_operations(5, 0);
  std::vector<double> durations(5, 0.0);

  const unsigned int SEED = 42;
  std::mt19937 gen;

  for (size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED);
    Timer timer;

    // 预热部分数据（只加载 20% 的数据）
    for (int key = 0; key < LOOP_SIZE / 5; ++key) {
      std::string value = "loop" + std::to_string(key);
      caches[i]->put(key, value);
    }

    int current_pos = 0;

    // 20% 写操作，80% 读操作
    for (int op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 20);
      int key;

      if (op % 100 < 60) { // 60% 顺序扫描
        key = current_pos;
        current_pos = (current_pos + 1) % LOOP_SIZE;
      } else if (op % 100 < 90) { // 30% 随机跳跃
        key = gen() % LOOP_SIZE;
      } else { // 10% 访问范围外数据
        key = LOOP_SIZE + (gen() % LOOP_SIZE);
      }

      if (isPut) {
        std::string value =
            "loop" + std::to_string(key) + "_v" + std::to_string(op % 100);
        caches[i]->put(key, value);
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("循环扫描测试", CAPACITY, get_operations, hits, durations);
}

// 测试场景3：工作负载剧烈变化测试
void testWorkloadShift() {
  std::cout << "\n=== 测试场景3：工作负载剧烈变化测试 ===" << std::endl;

  const int CAPACITY = 30;                 // 缓存容量
  const int OPERATIONS = 80000;            // 总操作次数
  const int PHASE_LENGTH = OPERATIONS / 5; // 每个阶段的长度（16000 次操作）

  YsCache::LruCache<int, std::string> lru(CAPACITY);
  YsCache::LfuCache<int, std::string> lfu(CAPACITY);
  YsCache::ArcCache<int, std::string> arc(CAPACITY);
  YsCache::LruKCache<int, std::string> lruk(CAPACITY, 500, 2);
  YsCache::LfuCache<int, std::string> lfuAging(CAPACITY, 10000);

  std::array<YsCache::YsCachePolicy<int, std::string> *, 5> caches = {
      &lru, &lfu, &arc, &lruk, &lfuAging};
  std::vector<int> hits(5, 0);
  std::vector<int> get_operations(5, 0);
  std::vector<double> durations(5, 0.0);

  const unsigned int SEED = 42;
  std::mt19937 gen;

  for (size_t i = 0; i < caches.size(); ++i) {
    gen.seed(SEED);
    Timer timer;

    // 预热缓存，插入少量初始数据
    for (int key = 0; key < 30; ++key) {
      std::string value = "init" + std::to_string(key);
      caches[i]->put(key, value);
    }

    // 进行多阶段动态测试
    for (int op = 0; op < OPERATIONS; ++op) {
      int phase = op / PHASE_LENGTH;

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
        key = gen() % 5;
      } else if (op < PHASE_LENGTH * 2) { // 阶段2: 大范围随机 (400 个键)
        key = gen() % 400;
      } else if (op < PHASE_LENGTH * 3) { // 阶段3: 顺序扫描 (100 个键)
        key = (op - PHASE_LENGTH * 2) % 100;
      } else if (op < PHASE_LENGTH * 4) { // 阶段4: 局部性随机 (5 个局部区域)
        int locality = (op / 800) % 5;
        key = locality * 15 + (gen() % 15);
      } else { // 阶段5: 混合访问
        int r = gen() % 100;
        if (r < 40) {
          key = gen() % 5; // 40% 热点
        } else if (r < 70) {
          key = 5 + (gen() % 45); // 30% 中等范围
        } else {
          key = 50 + (gen() % 350); // 30% 大范围
        }
      }

      if (isPut) {
        std::string value =
            "value" + std::to_string(key) + "_p" + std::to_string(phase);
        caches[i]->put(key, value);
      } else {
        get_operations[i]++;
        if (caches[i]->get(key).has_value()) {
          hits[i]++;
        }
      }
    }
    durations[i] = timer.elapsed();
  }

  printResults("工作负载剧烈变化测试", CAPACITY, get_operations, hits, durations);
}

int main() {
  testHotDataAccess();
  testLoopPattern();
  testWorkloadShift();
  return 0;
}
