#pragma once
#include "YsCachePolicy.h"
#include <atomic>
#include <iomanip>
#include <iostream>
#include <memory>

namespace YsCache {
template <typename Key, typename Value>
class MetricsCacheDecorator : public YsCachePolicy<Key, Value> {
public:
  using EvictionCallback = typename YsCachePolicy<Key, Value>::EvictionCallback;
  explicit MetricsCacheDecorator(
      std::unique_ptr<YsCachePolicy<Key, Value>> target)
      : target_(std::move(target)) {
    if (!target_) {
      throw std::invalid_argument("target cache cannot be null");
    }
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    totalRequests_++;

    auto result = target_->get(key);
    if (result.has_value()) {
      hits_++;
    } else {
      misses_++;
    }
    return result;
  }

  void put(Key key, Value value) override {
    puts_++;
    target_->put(std::move(key), std::move(value));
  }

  [[nodiscard]] bool remove(const Key &key) override {
    return target_->remove(key);
  }

  void setEvictionCallback(EvictionCallback cb) override {
    target_->setEvictionCallback(std::move(cb));
  }

  [[nodiscard]] bool contains(const Key &key) const override {
    return target_->contains(key);
  }

  [[nodiscard]] std::size_t size() const override { return target_->size(); }

  [[nodiscard]] std::size_t capacity() const noexcept override {
    return target_->capacity();
  }

  [[nodiscard]] bool empty() const override { return target_->empty(); }

  void clear() override { target_->clear(); }

  [[nodiscard]] std::size_t totalRequests() const noexcept {
    return totalRequests_;
  }
  [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
  [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
  [[nodiscard]] std::size_t puts() const noexcept { return puts_; }

  [[nodiscard]] double hitRate() const noexcept {
    std::size_t total = totalRequests_;
    return total > 0 ? (100.0 * hits_ / total) : 0.0;
  }

  void printMetrics(const std::string &title = "Cache Metrics") const {
    std::cout << "=== " << title << " ===" << std::endl;
    std::cout << "Total Requests : " << totalRequests_ << std::endl;
    std::cout << "Hits           : " << hits_ << std::endl;
    std::cout << "Misses         : " << misses_ << std::endl;
    std::cout << "Hit Rate       : " << std::fixed << std::setprecision(2)
              << hitRate() << " %" << std::endl;
  }

private:
  std::unique_ptr<YsCachePolicy<Key, Value>> target_;

  std::atomic<std::size_t> totalRequests_{0};
  std::atomic<std::size_t> hits_{0};
  std::atomic<std::size_t> misses_{0};
  std::atomic<std::size_t> puts_{0};
};
} // namespace YsCache
