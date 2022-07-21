#ifndef OPEN_RPC_hpp
#define OPEN_RPC_hpp

#include <concepts>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace std::literals;

namespace ORPC {
template <typename> struct impossible_t;
template <typename T> inline constexpr auto impossible = impossible_t<T>::value;

template <typename...> struct proxy {};
template <typename T> struct proxy<T> { using type = T; };

template <typename, template <typename...> typename>
inline constexpr bool is_specialisation = false;
template <template <typename...> typename T, typename... Args>
inline constexpr bool is_specialisation<T<Args...>, T> = true;

template <typename T>
concept tuple_like = requires {
  std::tuple_size<T>::value;
};

template <typename... Fs> struct overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs> overloaded(Fs...) -> overloaded<Fs...>;

template <typename F> inline constexpr bool is_standalone_functor = false;

template <typename F>
requires(std::is_member_function_pointer_v<
         decltype(&F::
                  operator())>) inline constexpr bool is_standalone_functor<F> =
    true;

template <typename Ret, typename... Args>
inline constexpr bool is_standalone_functor<Ret(Args...)> = true;

template <typename F>
requires(std::is_pointer_v<F>) inline constexpr bool is_standalone_functor<F> =
    is_standalone_functor<std::remove_pointer_t<F>>;

template <typename F>
requires(
    std::is_const_v<F> || std::is_volatile_v<F> ||
    std::is_reference_v<F>) inline constexpr bool is_standalone_functor<F> =
    is_standalone_functor<std::remove_cvref_t<F>>;

template <typename F>
concept standalone_functor = is_standalone_functor<F>;

template <typename F>
inline constexpr auto arguments_for_standalone_functor = impossible<F>;

template <typename T>
inline constexpr auto arguments_for_member_function = impossible<T>;

template <typename F>
requires(
    std::is_member_function_pointer_v<
        decltype(&F::
                 operator())>) inline constexpr auto arguments_for_standalone_functor<F> =
    arguments_for_member_function<decltype(&F::operator())>;

template <typename Ret, typename... Args>
inline constexpr auto arguments_for_standalone_functor<Ret(Args...)> =
    proxy<std::remove_cvref_t<Args>...>{};

template <typename Ret, typename... Args>
inline constexpr auto arguments_for_standalone_functor<Ret(Args...) const> =
    arguments_for_standalone_functor<Ret(Args...)>;

template <typename Ret, typename... Args>
inline constexpr auto arguments_for_standalone_functor<Ret(Args...) volatile> =
    arguments_for_standalone_functor<Ret(Args...)>;

template <typename Ret, typename... Args>
inline constexpr auto
    arguments_for_standalone_functor<Ret(Args...) const volatile> =
        arguments_for_standalone_functor<Ret(Args...)>;

template <typename F>
requires(std::is_const_v<F> || std::is_volatile_v<F> ||
         std::is_reference_v<
             F>) inline constexpr auto arguments_for_standalone_functor<F> =
    arguments_for_standalone_functor<std::remove_cvref_t<F>>;

template <typename F>
requires(std::is_pointer_v<
         F>) inline constexpr auto arguments_for_standalone_functor<F> =
    arguments_for_standalone_functor<std::remove_pointer_t<F>>;

template <typename T, typename U>
inline constexpr auto arguments_for_member_function<U(T::*)> =
    arguments_for_standalone_functor<U>;

template <typename T> inline constexpr char type_tag_for_type = impossible<T>;
template <> inline constexpr char type_tag_for_type<int> = 'i';
template <> inline constexpr char type_tag_for_type<std::string_view> = 's';
template <>
inline constexpr char type_tag_for_type<std::span<char const>> = 'b';

template <typename... Args>
inline constexpr char type_tags_for_args_storage[] = {
    type_tag_for_type<Args>..., '\0'};

template <typename... Args>
inline constexpr std::string_view type_tags_for_args =
    std::string_view{type_tags_for_args_storage<Args...>};

template <typename F>
inline constexpr std::string_view type_tags_for_standalone_functor =
    []<typename... Args>(proxy<Args...>) {
      return type_tags_for_args<Args...>;
    }(arguments_for_standalone_functor<F>);

template <typename F>
inline constexpr std::string_view type_tags_for_member_function =
    []<typename... Args>(proxy<Args...>) {
      return type_tags_for_args<Args...>;
    }(arguments_for_member_function<F>);

template <typename> struct serialiser;
template <typename T> serialiser(T) -> serialiser<T>;

template <> struct serialiser<int> {
  int value;

  friend auto operator<<(std::ostream &os, serialiser const &x)
      -> std::ostream & {
    os.put(static_cast<char>((x.value >> 24) & 0xFF));
    os.put(static_cast<char>((x.value >> 16) & 0xFF));
    os.put(static_cast<char>((x.value >> 8) & 0xFF));
    os.put(static_cast<char>(x.value & 0xFF));
    return os;
  }
};

template <> struct serialiser<std::string_view> {
  std::string_view value;

  friend auto operator<<(std::ostream &os, serialiser const &x)
      -> std::ostream & {
    return os << serialiser<int>{static_cast<int>(x.value.size())} << x.value;
  }
};

template <> struct serialiser<std::string> : serialiser<std::string_view> {};

template <> struct serialiser<std::span<char const>> {
  std::span<char const> value;

  friend auto operator<<(std::ostream &os, serialiser const &x)
      -> std::ostream & {
    os << serialiser<int>{static_cast<int>(x.value.size())};
    for (auto c : x.value) {
      os.put(c);
    }
    return os;
  }
};

template <>
struct serialiser<std::vector<char>> : serialiser<std::span<char const>> {};

struct insufficient_data {};

template <typename> struct parser;
template <typename T> parser(T) -> parser<T>;

template <> struct parser<int> {
  int &value;

  friend auto operator>>(std::istream &is, parser x) -> std::istream & {
    auto y0 = static_cast<uint8_t>(is.get());
    auto y1 = static_cast<uint8_t>(is.get());
    auto y2 = static_cast<uint8_t>(is.get());
    auto y3 = static_cast<uint8_t>(is.get());
    x.value = static_cast<int32_t>(y0 << 24 | y1 << 16 | y2 << 8 | y3);
    return is;
  }

  static auto from(std::span<char const> &bytes, std::size_t &bytes_read)
      -> int {
    if (bytes.size() < 4) {
      throw insufficient_data{};
    }
    auto value = static_cast<int32_t>(static_cast<uint8_t>(bytes[0]) << 24 |
                                      static_cast<uint8_t>(bytes[1]) << 16 |
                                      static_cast<uint8_t>(bytes[2]) << 8 |
                                      static_cast<uint8_t>(bytes[3]));
    bytes_read += 4;
    bytes = bytes.subspan(4);
    return value;
  }
};

template <> struct parser<std::string> {
  std::string &value;

  friend auto operator>>(std::istream &is, parser x) -> std::istream & {
    int size;
    is >> parser<int>{size};
    std::cerr << "Reading string of length " << size << '\n';
    x.value.resize(static_cast<std::size_t>(size));
    is.read(x.value.data(), size);
    if (is.gcount() != size) {
      std::cerr << "Only could read " << is.gcount() << " wanted " << size
                << '\n';
    }
    return is;
  }
};

template <> struct parser<std::string_view> {
  static auto from(std::span<char const> &bytes, std::size_t &bytes_read)
      -> std::string_view {
    auto size = static_cast<std::size_t>(parser<int>::from(bytes, bytes_read));
    if (size > bytes.size()) {
      throw insufficient_data{};
    }
    auto value = std::string_view{bytes.data(), size};
    bytes = bytes.subspan(size);
    bytes_read += size;
    return value;
  }
};

template <> struct parser<std::vector<char>> {
  std::vector<char> &value;

  friend auto operator>>(std::istream &is, parser x) -> std::istream & {
    int size;
    is >> parser<int>{size};
    std::cerr << "Reading blob of length " << size << '\n';
    x.value.resize(static_cast<std::size_t>(size));
    is.read(x.value.data(), size);
    if (is.gcount() != size) {
      std::cerr << "Only could read " << is.gcount() << " wanted " << size
                << '\n';
    }
    return is;
  }
};

template <> struct parser<std::span<char const>> {
  static auto from(std::span<char const> &bytes, std::size_t &bytes_read)
      -> std::span<char const> {
    auto size = static_cast<std::size_t>(parser<int>::from(bytes, bytes_read));
    if (size > bytes.size()) {
      throw insufficient_data{};
    }
    auto value = std::span{bytes.data(), size};
    bytes = bytes.subspan(size);
    bytes_read += size;
    return value;
  }
};

class argument_view {
private:
  std::variant<int, std::string_view, std::span<char const>> value;

public:
  argument_view(int x) : value{x} {}
  argument_view(std::string_view x) : value{x} {}
  argument_view(std::span<char const> x) : value{x} {}

  argument_view(std::string const &x) : argument_view{std::string_view{x}} {}

  argument_view(char type_tag, std::span<char const> &bytes,
                std::size_t &bytes_read) {
    switch (type_tag) {
    case 'i':
      value = parser<int>::from(bytes, bytes_read);
      break;
    case 's':
      value = parser<std::string_view>::from(bytes, bytes_read);
      break;
    case 'b':
      value = parser<std::span<char const>>::from(bytes, bytes_read);
      break;
    default:
      std::cout << "Invalid type tag: " << type_tag << '\n';
      break;
    }
  }

  static auto type_tag(argument_view arg) -> char {
    switch (arg.value.index()) {
    case 0:
      return 'i';
    case 1:
      return 's';
    case 2:
      return 'b';
    default:
      std::cerr << "Invalid variant\n";
      return ' ';
    }
  }

  friend auto operator<<(std::ostream &os, argument_view const &arg)
      -> std::ostream &;

  template <typename> friend struct argument_to_native_t;
};

inline auto operator<<(std::ostream &os, argument_view const &arg)
    -> std::ostream & {
  return std::visit(
      [&](auto const &x) -> std::ostream & { return os << serialiser{x}; },
      arg.value);
}

class argument {
private:
  std::variant<int, std::string, std::vector<char>> value;

public:
  argument(int x) : value{x} {}
  argument(std::string x) : value{x} {}
  argument(std::vector<char> x) : value{x} {}

  argument(char type_tag) {
    switch (type_tag) {
    case 'i':
      value = 0;
      break;
    case 's':
      value = ""s;
      break;
    case 'b':
      value = std::vector<char>{};
      break;
    default:
      std::cout << "Invalid type tag: " << type_tag << '\n';
      break;
    }
  }

  auto view() -> argument_view {
    return std::visit([](auto const &x) { return argument_view{x}; }, value);
  }
  operator argument_view() { return view(); }

  friend auto operator>>(std::istream &is, argument &arg) -> std::istream & {
    std::visit([&](auto &x) -> std::istream & { return is >> parser{x}; },
               arg.value);
    return is;
  }
};

template <typename T> struct argument_to_native_t {
  static auto convert(argument_view arg) -> T {
    if (std::holds_alternative<T>(arg.value)) {
      return std::get<T>(arg.value);
    } else {
      std::cerr << "Incorrect argument type\n";
      return {};
    }
  }
};

template <typename F> auto argument_to_native(argument_view arg) {
  return argument_to_native_t<F>::convert(arg);
}

struct message_view {
  std::string_view address;
  std::vector<argument_view> arguments;

  static auto from(std::span<char const> &bytes, std::size_t &bytes_read)
      -> message_view {
    auto msg = message_view();

    msg.address = parser<std::string_view>::from(bytes, bytes_read);

    auto type_tags_with_delim =
        parser<std::string_view>::from(bytes, bytes_read);

    if (type_tags_with_delim.size() == 0 || type_tags_with_delim[0] != ',') {
      std::cerr << "Invalid type tag string: " << type_tags_with_delim
                << " for address: " << msg.address << ", entering UB\n";
    } else {
      auto type_tags = type_tags_with_delim.substr(1);

      for (auto type_tag : type_tags) {
        msg.arguments.emplace_back(type_tag, bytes, bytes_read);
      }
    }

    return msg;
  }

  auto type_tags() const -> std::string {
    return std::transform_reduce(arguments.begin(), arguments.end(), ""s,
                                 std::plus{}, argument_view::type_tag);
  }
};

inline auto operator<<(std::ostream &os, message_view const &msg)
    -> std::ostream & {
  os << serialiser{msg.address} << serialiser{"," + msg.type_tags()};
  std::copy(msg.arguments.begin(), msg.arguments.end(),
            std::ostream_iterator<argument_view>{os});
  return os;
}

struct message {
  std::string address;
  std::vector<argument> arguments;

  auto view() -> message_view {
    return {address, {arguments.begin(), arguments.end()}};
  }
  operator message_view() { return view(); }

  friend auto operator>>(std::istream &is, message &msg) -> std::istream & {
    is >> parser{msg.address};

    std::string type_tags_with_delim;
    is >> parser{type_tags_with_delim};

    if (type_tags_with_delim.size() == 0 || type_tags_with_delim[0] != ',') {
      std::cerr << "Invalid type tag string: " << type_tags_with_delim
                << " for address: " << msg.address << " entering UB\n";
    } else {
      auto type_tags = std::string_view{type_tags_with_delim}.substr(1);

      for (auto type_tag : type_tags) {
        is >> msg.arguments.emplace_back(type_tag);
      }
    }

    return is;
  }
};

inline auto split_address(std::string_view address)
    -> std::optional<std::pair<std::string_view, std::string_view>> {
  if (!address.starts_with('/')) {
    std::cerr << "Invalid address: \"" << address << "\"\n";
    return std::nullopt;
  } else {
    auto sep_index = address.find('/', 1);
    auto address_head = address.substr(1, sep_index - 1); // TODO, check this
    auto address_tail =
        sep_index == std::string_view::npos ? ""sv : address.substr(sep_index);
    return std::pair{address_head, address_tail};
  }
}

template <typename T>
auto lookup_address(std::string_view address,
                    std::unordered_map<std::string, T> const &map)
    -> std::optional<std::pair<T const &, std::string_view>> {
  if (auto address_head_tail = split_address(address)) {
    auto [address_head, address_tail] = *address_head_tail;
    // TODO this string copy is nasty
    if (map.contains(std::string{address_head})) {
      return std::pair<T const &, std::string_view>{
          map.at(std::string{address_head}), address_tail};
    } else {
      std::cerr << "Unknown address: " << address_head << "\n";
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
}

template <typename Parent> class member_interface {
private:
  std::function<void(message_view const &)> callback;

protected:
  struct non_member {};

  static constexpr bool is_member = !std::is_void_v<Parent>;

  using parent_ref = typename decltype([] {
    if constexpr (is_member) {
      if constexpr (std::is_function_v<Parent>) {
        return proxy<std::function<Parent>>{};
      } else {
        return proxy<Parent &>{};
      }
    } else {
      return proxy<non_member>{};
    }
  }())::type;

public:
  void set_callback(std::function<void(message_view const &)> _callback) {
    callback = std::move(_callback);
  }

  void send(message_view const &msg) const {
    if (callback) {
      callback(msg);
    } else {
      std::cerr << "Sending to empty callback\n";
    }
  }

  virtual ~member_interface() = default;

  virtual void process(message_view const &, parent_ref = non_member{}) = 0;
};

using interface = member_interface<void>;

template <typename Parent, std::derived_from<interface> T>
class erase_parent : public member_interface<Parent> {
private:
  T wrapped;

public:
  erase_parent(erase_parent const &) = delete;

  erase_parent(auto &&...args)
      : wrapped{std::forward<decltype(args)>(args)...} {
    wrapped.set_callback([this](message_view const &msg) { this->send(msg); });
  }

  void process(message_view const &msg,
               typename erase_parent::parent_ref =
                   typename erase_parent::non_member{}) override {
    return wrapped.process(msg);
  }
};

template <typename T>
concept member_pointer = std::is_member_pointer_v<std::remove_cvref_t<T>>;

template <typename T, std::size_t I>
concept has_orpc_member = requires {
  {
    std::get<I>(std::remove_cvref_t<T>::orpc_members).first
    } -> std::convertible_to<std::string>;
  {
    std::get<I>(std::remove_cvref_t<T>::orpc_members).second
    } -> member_pointer;
};

template <typename, typename> constexpr bool has_orpc_members_indexed = false;

template <typename T, std::size_t... Is>
constexpr bool has_orpc_members_indexed<T, std::index_sequence<Is...>> =
    (has_orpc_member<T, Is> && ...);

template <typename T>
concept has_orpc_members = has_orpc_members_indexed<
    T, std::make_index_sequence<std::tuple_size_v<decltype(T::orpc_members)>>>;

template <typename Parent, typename T>
class subobject : public member_interface<std::remove_reference_t<Parent>> {
private:
  std::unordered_map<
      std::string,
      std::unique_ptr<member_interface<std::remove_reference_t<T>>>>
      children;

  struct stateless {};

  using value_t = typename decltype([] {
    if constexpr (subobject::is_member) {
      return proxy<T(std::remove_reference_t<Parent>::*)>{};
    } else if constexpr (std::is_void_v<T>) {
      return proxy<stateless>{};
    } else {
      return proxy<T>{};
    }
  }())::type;

  value_t value;

  auto get_value(typename subobject::parent_ref parent) -> decltype(auto) {
    if constexpr (subobject::is_member) {
      if constexpr (std::is_function_v<T>) {
        return [&parent, value = this->value](auto &&...args) mutable {
          return (parent.*value)(std::forward<decltype(args)>(args)...);
        };
      } else {
        return (parent.*value);
      }
    } else {
      if constexpr (std::is_void_v<T>) {
        return typename subobject::non_member{};
      } else {
        return (value);
      }
    }
  }

  template <typename F>
  requires(std::is_function_v<F>) auto make_interface(F const &f) {
    return make_interface(&f);
  }

  template <typename F> auto make_interface(F &f) {
    if constexpr (std::is_void_v<T>) {
      return std::make_unique<subobject<void, F &>>(f);
    } else {
      return std::make_unique<
          erase_parent<std::remove_reference_t<T>, subobject<void, F &>>>(f);
    }
  }

  template <typename F> auto make_interface(F &&f) {
    if constexpr (std::is_void_v<T>) {
      return std::make_unique<subobject<void, std::remove_cvref_t<F>>>(
          std::forward<F>(f));
    } else {
      return std::make_unique<erase_parent<
          std::remove_reference_t<T>, subobject<void, std::remove_cvref_t<F>>>>(
          std::forward<F>(f));
    }
  }

  template <typename U, std::same_as<std::remove_cvref_t<T>> T2>
  auto make_interface(U T2::*x) {
    return std::make_unique<subobject<T, U>>(x);
  }

  template <typename U>
  requires(
      std::derived_from<std::remove_cvref_t<U>,
                        interface>) auto make_interface(std::unique_ptr<U> x) {
    return std::move(x);
  }

  void send_result(message_view const &, auto &&f) requires(
      std::is_void_v<std::invoke_result_t<decltype(f)>>) {
    f();
  }

  void send_result(message_view const &msg, auto &&f) requires(
      std::is_convertible_v<std::invoke_result_t<decltype(f)>, argument_view>) {
    this->send({msg.address, {f()}});
  }

  void send_result(message_view const &msg, auto &&f) requires(
      tuple_like<std::invoke_result_t<decltype(f)>>) {
    auto args = f();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) mutable {
      this->send({msg.address, {std::move(std::get<Is>(args))...}});
    }
    (std::make_index_sequence<std::tuple_size_v<decltype(args)>>{});
  }

  void send_result(message_view const &msg, auto &&f) requires(
      is_specialisation<std::invoke_result_t<decltype(f)>, std::vector>) {
    auto args = f();
    this->send({msg.address, {args.begin(), args.end()}});
  }

  void call(message_view const &msg, typename subobject::parent_ref parent =
                                         typename subobject::non_member{}) {
    send_result(msg, [&] {
      return [&]<typename... Args>(proxy<Args...>) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
          return get_value(parent)(
              argument_to_native<Args>(msg.arguments[Is])...);
        }
        (std::index_sequence_for<Args...>{});
      }(arguments_for_standalone_functor<T>);
    });
  }

public:
  subobject(subobject const &) = delete;

  subobject(value_t value = stateless{}) : value{value} {
    if constexpr (has_orpc_members<std::remove_cvref_t<T>>) {
      [this]<std::size_t... Is>(std::index_sequence<Is...>) {
        (
            [this] {
              auto [address, member] =
                  std::get<Is>(std::remove_cvref_t<T>::orpc_members);
              add(address, member);
            }(),
            ...);
      }
      (std::make_index_sequence<
          std::tuple_size_v<decltype(std::remove_cvref_t<T>::orpc_members)>>());
    }
  }

  void process(message_view const &msg,
               typename subobject::parent_ref parent =
                   typename subobject::non_member{}) override {
    if (msg.address == ""sv) {
      if constexpr (is_standalone_functor<T>) {
        static constexpr auto type_tags = type_tags_for_standalone_functor<T>;
        if (msg.type_tags() != type_tags) {
          std::cerr << "Incorrect argument types: " << msg.type_tags()
                    << ", expected: " << type_tags << "\n";
        } else {
          call(msg, parent);
        }
      } else {
        std::cerr << "Cannot stop on a non-callable endpoint\n";
      }
    } else if (auto child_address_tail =
                   lookup_address(msg.address, this->children)) {
      auto [child, address_tail] = *child_address_tail;
      child->process({address_tail, msg.arguments}, get_value(parent));
    }
  }

  auto add(std::string address, auto &&x) -> auto & {
    auto interface_owner = make_interface(std::forward<decltype(x)>(x));
    auto interface_view = interface_owner.get();
    this->children.emplace(address, std::move(interface_owner));

    // TODO copy?
    interface_view->set_callback([this, address](message_view const &msg) {
      this->send({"/"s + address + std::string{msg.address}, msg.arguments});
    });

    return *interface_view;
  }
};

using router = subobject<void, void>;

template <typename T> auto object(T &value) {
  return subobject<void, T>{value};
}

class stream_child : public interface {
private:
  std::istream &is;
  std::ostream &os;

  std::thread reader;

public:
  stream_child(stream_child const &) = delete;

  stream_child(std::istream &is, std::ostream &os)
      : is{is}, os{os}, reader{[this] {
          // TODO stop
          while (true) {
            message msg;
            this->is >> msg;
            this->send(msg);
          }
        }} {}

  void process(message_view const &msg,
               stream_child::non_member = stream_child::non_member{}) override {
    os << msg << std::flush;
  }
};

template <std::derived_from<interface> Child> class stream_parent {
private:
  std::istream &is;
  std::ostream &os;

  Child child;

  std::thread reader;

public:
  stream_parent(stream_parent const &) = delete;

  stream_parent(std::istream &is, std::ostream &os, auto &&...args)
      : is{is}, os{os}, child{std::forward<decltype(args)>(args)...},
        reader{[this] {
          // TODO stop
          while (true) {
            message msg;
            this->is >> msg;
            this->child.process(msg);
          }
        }} {
    child.set_callback(
        [this](message_view msg) { this->os << msg << std::flush; });
  }

  auto operator->() -> Child * { return &child; }
  auto operator->() const -> Child const * { return &child; }
};
} // namespace ORPC

#endif // OPEN_RPC_hpp
