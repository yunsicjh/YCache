#pragma once

#include <cstddef>
#include <functional>
#include <optional>
namespace YsCache {

/**
 * @brief 缓存策略抽象基类（纯虚接口）
 * @tparam Key 缓存键类型
 * @tparam Value 缓存值类型
 *
 * 工业级设计原则：
 * 1. 纯虚接口定义最小正交集合（Orthogonal Interface）；
 * 2. 入参采用 Sink Argument 传值模式，对右值提供零多余拷贝支持；
 * 3. 查询与状态判断接口标有 [[nodiscard]]，防止逻辑误用（如误把 empty() 当作
 * clear()）；
 * 4. 读操作全量具备 const 规范，内部并发安全由派生类的 mutable 同步原语支撑；
 * 5. 严格区分宽契约与强 noexcept 边界，为多线程锁和自定义类型留出安全弹性。
 */
template <typename Key, typename Value> class YsCachePolicy {
public:
  using EvictionCallback = std::function<void(const Key &, const Value &)>;
  virtual ~YsCachePolicy() = default;

  // ---------------- 核心读写接口 ----------------

  /**
   * @brief 写入或更新缓存（Sink 传值模式）
   * @param key 键（值传递，支持外部完美转移或局部拷贝）
   * @param value 值（值传递）
   */
  virtual void put(Key key, Value value) = 0;

  /**
   * @brief 读取缓存
   * @param key 键
   * @return 若命中返回包含 Value 的 optional，否则返回 std::nullopt
   */
  [[nodiscard]] virtual std::optional<Value> get(const Key &key) = 0;

  /**
   * @brief 从缓存中显式移除指定的键
   * @param key 键
   * @return 存在并移除成功返回 true，键不存在返回 false
   */
  [[nodiscard]] virtual bool remove(const Key &key) = 0;
  // ---------------- 观察者模式监听接口 ----------------
  /**
   * @brief 注册淘汰监听器（观察者）
   * @param cb 监听回调函数
   */
  virtual void setEvictionCallback(EvictionCallback cb) {
    (void)cb; // 默认空实现
  }
  // ---------------- 基础状态与运维查询接口 ----------------

  /**
   * @brief 检查键是否存在于缓存中（纯查询，不修改访问频次或热度）
   * @param key 键
   */
  [[nodiscard]] virtual bool contains(const Key &key) const = 0;

  /**
   * @brief 清空当前所有缓存数据
   */
  virtual void clear() = 0;

  /**
   * @brief 获取当前缓存中存储的有效元素数量
   */
  [[nodiscard]] virtual std::size_t size() const = 0;

  /**
   * @brief 获取缓存的最大容量上限（配置项只读，强 noexcept）
   */
  [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;

  /**
   * @brief 判断缓存是否为空
   */
  [[nodiscard]] virtual bool empty() const = 0;
};

} // namespace YsCache
