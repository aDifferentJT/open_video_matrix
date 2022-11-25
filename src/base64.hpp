#ifndef BASE64_HPP
#define BASE64_HPP

#include <array>
#include <string>
#include <string_view>

using namespace std::literals;

template <typename T, T From, T To, T... Ints>
auto integer_sequence_half_open_range_helper(std::integer_sequence<T, Ints...>)
    -> std::integer_sequence<T, (Ints + From)...> {
  return {};
}

template <typename T, T From, T To>
using integer_sequence_half_open_range =
    decltype(integer_sequence_half_open_range_helper<T, From, To>(
        std::make_integer_sequence<T, To - From>{}));

template <typename T, T From, T To>
using integer_sequence_closed_range =
    integer_sequence_half_open_range<T, From, To + 1>;

constexpr auto base64_table = [] {
  return []<char... A_Z, char... a_z, char... _0_9>(
      std::integer_sequence<char, A_Z...>, std::integer_sequence<char, a_z...>,
      std::integer_sequence<char, _0_9...>) {
    return std::array<char, 64>{A_Z..., a_z..., _0_9..., '+', '/'};
  }
  (integer_sequence_closed_range<char, 'A', 'Z'>{},
   integer_sequence_closed_range<char, 'a', 'z'>{},
   integer_sequence_closed_range<char, '0', '9'>{});
}();

constexpr auto base64(char const *data, long size) -> std::string {
  auto ret = std::string{};
  ret.reserve(static_cast<std::size_t>((size - 1) * 4 / 3 + 1));

  while (size > 0) {
    auto block = [&]() -> std::array<unsigned char, 3> {
      switch (size) {
      case 0:
        throw 0;
      case 1:
        return {static_cast<unsigned char>(data[0])};
      case 2:
        return {static_cast<unsigned char>(data[0]),
                static_cast<unsigned char>(data[1])};
      default:
        return {static_cast<unsigned char>(data[0]),
                static_cast<unsigned char>(data[1]),
                static_cast<unsigned char>(data[2])};
      }
    }();

    ret.push_back(base64_table[0b111111 & (block[0] >> 2)]);
    ret.push_back(base64_table[0b111111 & (block[0] << 4 | block[1] >> 4)]);
    ret.push_back(base64_table[0b111111 & (block[1] << 2 | block[2] >> 6)]);
    ret.push_back(base64_table[0b111111 & (block[2])]);

    data += 3;
    size -= 3;
  }

  switch (size) {
  case -2:
    ret[ret.size() - 2] = '=';
    [[fallthrough]];
  case -1:
    ret[ret.size() - 1] = '=';
    [[fallthrough]];
  default:
    break;
  }

  return ret;
}

constexpr auto base64(std::string_view str) {
  return base64(str.data(), static_cast<long>(str.size()));
}

#endif // BASE64_HPP

// Outside ifdef to verify other instance

/* Comment out pending constexpr std::string
static_assert(base64("Man"sv) == "TWFu"sv);

static_assert(base64("Ma"sv) == "TWE="sv);

static_assert(base64("M"sv) == "TQ=="sv);

static_assert(base64("Many hands make light work."sv) ==
              "TWFueSBoYW5kcyBtYWtlIGxpZ2h0IHdvcmsu"sv);
*/
