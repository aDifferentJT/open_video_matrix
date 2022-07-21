#ifndef CONTROLLER_SYNCHRONISED_HPP
#define CONTROLLER_SYNCHRONISED_HPP

#include <mutex>
#include <type_traits>
#include <utility>

template <typename T> class locked {
private:
  T &value;
  std::scoped_lock<std::mutex> lock;

public:
  explicit locked(T &value, std::mutex &mutex) : value{value}, lock{mutex} {}

  auto get() -> T & { return value; }
  auto operator->() -> std::remove_reference_t<T> * { return &value; }
};

template <typename T> class synchronised {
private:
  T value;
  std::mutex mutex;

public:
  explicit synchronised(auto &&...args) : value{std::forward<decltype(args)>(args)...} {}

  auto lock() { return locked<T>{value, mutex}; }
  auto operator->() { return lock(); }
};

#endif // CONTROLLER_SYNCHRONISED_HPP
