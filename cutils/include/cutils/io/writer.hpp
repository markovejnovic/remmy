#ifndef CUTILS_IO_WRITER_HPP
#define CUTILS_IO_WRITER_HPP

#include <concepts>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace cutils::io {

template <class R>
concept PieceRange =
    std::ranges::forward_range<R> &&
    std::convertible_to<std::ranges::range_reference_t<R>, std::string_view> &&
    (std::is_lvalue_reference_v<std::ranges::range_reference_t<R>> ||
     std::same_as<std::ranges::range_reference_t<R>, std::string_view>);

/// @brief Type which supports writing to.
template <class T>
concept Writer = requires(T& a, std::string_view sv,
                          std::span<const std::string_view> pieces) {
  { a.Write(sv) } -> std::convertible_to<std::expected<std::size_t, std::errc>>;
  {
    a.WriteMany(pieces)
  } -> std::convertible_to<std::expected<void, std::errc>>;
  { a.Flush() } -> std::convertible_to<std::expected<void, std::errc>>;
};

}  // namespace cutils::io

#endif  // CUTILS_IO_WRITER_HPP
