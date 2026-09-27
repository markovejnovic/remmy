#ifndef CUTILS_IO_PRINT_HPP
#define CUTILS_IO_PRINT_HPP

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>

#include "cutils/io/writer.hpp"

namespace cutils::io {

template <class T>
struct Formatter;

template <class T>
  requires std::convertible_to<const T&, std::string_view>
struct Formatter<T> {
  explicit constexpr Formatter(const T& value) noexcept : view_(value) {}

  [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
    return view_;
  }

 private:
  std::string_view view_;
};

template <>
struct Formatter<char> {
  explicit constexpr Formatter(char value) noexcept : value_(value) {}
  Formatter(const Formatter&) = delete;
  Formatter(Formatter&&) = delete;
  auto operator=(const Formatter&) -> Formatter& = delete;
  auto operator=(Formatter&&) -> Formatter& = delete;
  ~Formatter() = default;

  [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
    return {&value_, 1};
  }

 private:
  char value_;
};

template <>
struct Formatter<bool> {
  explicit constexpr Formatter(bool value) noexcept : value_(value) {}

  [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
    return value_ ? "true" : "false";
  }

 private:
  bool value_;
};

template <std::integral T>
  requires(!std::same_as<T, char> && !std::same_as<T, bool>)
struct Formatter<T> {
  explicit constexpr Formatter(T value) noexcept {
    const std::to_chars_result result =
        std::to_chars(buf_.data(), buf_.data() + buf_.size(), value);
    len_ = static_cast<std::size_t>(result.ptr - buf_.data());
  }
  Formatter(const Formatter&) = delete;
  Formatter(Formatter&&) = delete;
  auto operator=(const Formatter&) -> Formatter& = delete;
  auto operator=(Formatter&&) -> Formatter& = delete;
  ~Formatter() = default;

  [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
    return {buf_.data(), len_};
  }

 private:
  std::array<char, std::numeric_limits<T>::digits10 + 2> buf_{};
  std::size_t len_{0};
};

template <class T>
concept Formattable = requires(const T& value) {
  { Formatter<T>(value).View() } -> std::same_as<std::string_view>;
};

namespace detail {

inline void InvalidFormatString() noexcept {}

}  // namespace detail

template <class... Args>
struct FormatString {
  template <class S>
    requires std::convertible_to<const S&, std::string_view>
  consteval FormatString(  // NOLINT(google-explicit-constructor)
      const S& s)
      : str(s) {
    std::size_t holes = 0;
    for (std::size_t i = 0; i < str.size(); ++i) {
      if (str[i] == '{' && i + 1 < str.size() && str[i + 1] == '}') {
        ++holes;
        ++i;
      } else if (str[i] == '{' || str[i] == '}') {
        detail::InvalidFormatString();
      }
    }
    if (holes != sizeof...(Args)) {
      detail::InvalidFormatString();
    }
  }

  std::string_view str;
};

namespace detail {

template <class... Args>
[[nodiscard]] constexpr auto FormatPieces(
    std::string_view fmt,
    const std::tuple<Formatter<Args>...>& formatted) noexcept
    -> std::array<std::string_view, (2 * sizeof...(Args)) + 1> {
  constexpr std::size_t kArgs = sizeof...(Args);
  const std::array<std::string_view, kArgs> views = std::apply(
      [](const auto&... f) {
        return std::array<std::string_view, kArgs>{f.View()...};
      },
      formatted);

  std::array<std::string_view, (2 * kArgs) + 1> pieces{};
  for (std::size_t i = 0; i < kArgs; ++i) {
    const std::size_t hole = fmt.find("{}");
    pieces[2 * i] = fmt.substr(0, hole);
    pieces[(2 * i) + 1] = views[i];
    fmt.remove_prefix(hole + 2);
  }
  pieces[2 * kArgs] = fmt;
  return pieces;
}

}  // namespace detail

template <Writer W, Formattable... Args>
[[nodiscard]] auto Print(W& writer,
                         FormatString<std::type_identity_t<Args>...> fmt,
                         const Args&... args) noexcept
    -> std::expected<void, std::errc> {
  const std::tuple<Formatter<Args>...> formatted(args...);
  return writer.WriteMany(detail::FormatPieces(fmt.str, formatted));
}

template <Writer W, Formattable... Args>
[[nodiscard]] auto PrintLn(W& writer,
                           FormatString<std::type_identity_t<Args>...> fmt,
                           const Args&... args) noexcept
    -> std::expected<void, std::errc> {
  static constexpr std::string_view kNewline = "\n";
  const std::tuple<Formatter<Args>...> formatted(args...);
  const auto pieces = detail::FormatPieces(fmt.str, formatted);
  return writer.WriteMany(
      std::array<std::span<const std::string_view>, 2>{pieces, {&kNewline, 1}} |
      std::views::join);
}

}  // namespace cutils::io

#endif  // CUTILS_IO_PRINT_HPP
