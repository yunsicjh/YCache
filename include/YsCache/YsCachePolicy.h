#pragma once
#include <optional>

namespace YsCache {

template <typename Key, typename Value> class YsCachePolicy {
public:
  virtual ~YsCachePolicy() = default;
  virtual void put(Key key, Value value) = 0;
  virtual std::optional<Value> get(const Key &key) = 0;
  virtual bool remove(const Key &key) = 0;
};
} // namespace YsCache
