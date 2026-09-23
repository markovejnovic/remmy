// See LICENSE in the repository root.

/// @brief Utilities for reading and deserializing environment variables.
///
/// @warn This was mostly vibe-coded but is human-reviewed and human designed.
#ifndef CUTILS_OS_ENV_HPP
#define CUTILS_OS_ENV_HPP

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

namespace cutils {

enum class EnvError : std::uint8_t { NotSet, Malformed, OutOfRange };

template <typename T>
struct EnvParser;

namespace detail {

[[nodiscard]] constexpr auto AsciiLower(char c) noexcept -> char {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr auto EqualsIgnoreCase(std::string_view lhs,
                                              std::string_view rhs) noexcept
    -> bool {
  return std::ranges::equal(
      lhs, rhs, [](char a, char b) { return AsciiLower(a) == AsciiLower(b); });
}

[[nodiscard]] constexpr auto MatchesAny(
    std::string_view text,
    std::initializer_list<std::string_view> spellings) noexcept -> bool {
  return std::ranges::any_of(spellings, [text](std::string_view spelling) {
    return EqualsIgnoreCase(text, spelling);
  });
}

}  // namespace detail

template <std::integral T>
  requires(!std::same_as<std::remove_cv_t<T>, bool>)
struct EnvParser<T> {
  [[nodiscard]] static constexpr auto Parse(std::string_view text) noexcept
      -> std::expected<T, EnvError> {
    T value{};
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [stopped, ec] = std::from_chars(first, last, value);

    if (ec == std::errc::result_out_of_range) {
      return std::unexpected(EnvError::OutOfRange);
    }

    if (ec != std::errc{} || stopped != last) {
      return std::unexpected(EnvError::Malformed);
    }

    return value;
  }
};

template <>
struct EnvParser<bool> {
  [[nodiscard]] static constexpr auto Parse(std::string_view text) noexcept
      -> std::expected<bool, EnvError> {
    if (detail::MatchesAny(text, {"1", "true", "yes", "on"})) {
      return true;
    }

    if (detail::MatchesAny(text, {"0", "false", "no", "off"})) {
      return false;
    }

    return std::unexpected(EnvError::Malformed);
  }
};

template <>
struct EnvParser<std::string_view> {
  [[nodiscard]] static constexpr auto Parse(std::string_view text) noexcept
      -> std::expected<std::string_view, EnvError> {
    return text;
  }
};

template <typename T>
[[nodiscard]] auto GetEnv(const char* name) -> std::expected<T, EnvError> {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* const raw = std::getenv(name);
  if (raw == nullptr) {
    return std::unexpected(EnvError::NotSet);
  }
  return EnvParser<T>::Parse(std::string_view{raw});
}

template <typename T>
[[nodiscard]] auto GetEnvOrDefault(const char* name, T fallback) -> T {
  return GetEnv<T>(name).value_or(std::move(fallback));
}

}  // namespace cutils

#endif
