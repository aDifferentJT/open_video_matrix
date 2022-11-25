#include <iostream>
#include <random>
#include <utility>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

namespace ipc = boost::interprocess;

using namespace std::literals;

template <typename T> class ipc_managed_object {
private:
  static auto generate_name() -> std::string {
    static constexpr auto chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"sv;

    auto name = std::string{};

    auto rng = std::random_device{};
    auto index_dist =
        std::uniform_int_distribution{std::size_t{0}, chars.size() - 1};
    for (int i = 0; i < 32; ++i) {
      name += chars[index_dist(rng)];
    }
    return name;
  }

  std::string _name = generate_name();

  struct remover_t {
    char const *name;
    ~remover_t() {
      if (!ipc::shared_memory_object::remove(name)) {
        std::cerr << "Failed to remove shared memory object\n";
      }
    }
  } remover;

  ipc::shared_memory_object object;
  ipc::mapped_region region;

public:
  ipc_managed_object(auto &&...args)
      : remover{_name.c_str()}, object{[&] {
          auto object = ipc::shared_memory_object{
              ipc::create_only, _name.c_str(), ipc::read_write};
          object.truncate(sizeof(T));
          return object;
        }()},
        region{object, ipc::read_write, 0, sizeof(T)} {
    std::construct_at<T>(data(), std::forward<decltype(args)>(args)...);
  }

  ~ipc_managed_object() { std::destroy_at<T>(data()); }

  auto name() const -> std::string const & { return _name; }

  auto data() -> T * { return reinterpret_cast<T *>(region.get_address()); }
  auto data() const -> T const * { return reinterpret_cast<T const *>(region.get_address()); }

  auto operator->() -> T * { return data(); }
};

template <typename T> class ipc_unmanaged_object {
private:
  ipc::shared_memory_object object;
  ipc::mapped_region region;

public:
  ipc_unmanaged_object(char const *name)
      : object{ipc::open_only, name, ipc::read_write}, region{object,
                                                              ipc::read_write,
                                                              0, sizeof(T)} {}

  auto data() -> T * { return reinterpret_cast<T *>(region.get_address()); }

  auto operator*() -> T & { return *data(); }

  auto operator->() -> T * { return data(); }
};
