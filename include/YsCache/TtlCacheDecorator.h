#pragma once
#include "YsCachePolicy.h"
#include <chrono>
#include <memory>
#include <optional>

namespace YsCache {
template <typename Key, typename Value>
class TtlCacheDecorator : public YsCachePolicy<Key, Value> {
public:
  struct TimedValue {
    Value value;
    std::chrono::steady_clock::time_point expireAt;
  };

  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::milliseconds;
  using UnderlyingCache = YsCachePolicy<Key, TimedValue>;
  using EvictionCallback =
      typename YsCachePolicy<Key, Value>::EvictionCallback;
  explicit TtlCacheDecorator(std::unique_ptr<UnderlyingCache> target,
                             Duration defaultTtl)
      : target_(std::move(target)), defaultTtl_(defaultTtl) {
    if (!target_) {
      throw std::invalid_argument("Target cache cannot be null");
    }
  }

  void put(Key key, Value value) override {
    putWithTtl(std::move(key), std::move(value), defaultTtl_);
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) override {
    auto res = target_->get(key);
    if (!res.has_value()) {
      return std::nullopt;
    }

    if (Clock::now() >= res->expireAt) {
      (void)target_->remove(key);
      return std::nullopt;
    }

    return std::move(res->value);
  }

  [[nodiscard]] bool remove(const Key &key) override {
    return target_->remove(key);
  }

  void setEvictionCallback(EvictionCallback cb) override {
    if (cb) {
      // 适配器模式：底层传出来的是 TimedValue，解包出真实 value 传递给外部回调
      target_->setEvictionCallback(
          [userCb = std::move(cb)](const Key &key, const TimedValue &tv) {
            userCb(key, tv.value);
          });
    } else {
      target_->setEvictionCallback(nullptr);
    }
  }
  [[nodiscard]] bool contains(const Key &key) const override {
    // contains 也必须履行过期检查逻辑
    auto res = target_->get(key);
    if (!res.has_value()) {
      return false;
    }
    if (Clock::now() >= res->expireAt) {
      // 已过期
      (void)const_cast<UnderlyingCache *>(target_.get())->remove(key);
      return false;
    }
    return true;
  }

  [[nodiscard]] std::size_t size() const override { return target_->size(); }

  [[nodiscard]] std::size_t capacity() const noexcept override {
    return target_->capacity();
  }

  [[nodiscard]] bool empty() const override { return target_->empty(); }

  void clear() override { target_->clear(); }

  [[nodiscard]] Duration defaultTtl() const noexcept { return defaultTtl_; }

private:
  void putWithTtl(Key key, Value value, Duration ttl) {
    auto expireAt = Clock::now() + ttl;
    target_->put(std::move(key), TimedValue{std::move(value), expireAt});
  }
  std::unique_ptr<UnderlyingCache> target_;
  Duration defaultTtl_;
};
} // namespace YsCache