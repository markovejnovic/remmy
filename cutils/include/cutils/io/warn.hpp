// See LICENSE in the repository root.
/// @todo I only had a cursory review through this. It looks mostly correct,
///       but is one-shot generated. It's copied from Apple's source, for the
///       most part anyways, and that's what LLMs are good for.

#ifndef CUTILS_IO_WARN_HPP
#define CUTILS_IO_WARN_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>

#include "cutils/io/print.hpp"
#include "cutils/io/writer.hpp"
#include "cutils/os/os.hpp"

namespace cutils::io {

namespace detail {

/// @brief Apple err(3)'s escape for each C0 byte; empty means print it raw.
inline constexpr std::array<std::string_view, 32> kWarnEscapes{
    "",      "\\001", "\\002", "\\003", "\\004", "\\005", "\\006", "\\a",
    "\\b",   "",      "",      "\\v",   "\\f",   "\\r",   "\\016", "\\017",
    "\\020", "\\021", "\\022", "\\023", "\\024", "\\025", "\\026", "\\027",
    "\\030", "\\031", "\\032", "\\033", "\\034", "\\035", "\\036", "\\037",
};

/// @brief The escape for `byte`, or empty when it is printed as is.
[[nodiscard]] constexpr auto WarnEscape(char byte) noexcept
    -> std::string_view {
  const auto value = static_cast<unsigned char>(byte);
  return value < kWarnEscapes.size() ? kWarnEscapes[value] : std::string_view{};
}

/// @brief Pieces of a warning: `head` and `tail` raw, `body` escaped.
class WarnPieces {
 public:
  constexpr WarnPieces(std::span<const std::string_view> head,
                       std::span<const std::string_view> body,
                       std::span<const std::string_view> tail) noexcept
      : parts_{head, body, tail} {}

  /// @brief Yields raw runs as views and escapes from kWarnEscapes.
  class iterator {
   public:
    using iterator_concept = std::forward_iterator_tag;
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;

    constexpr iterator() noexcept = default;

    [[nodiscard]] constexpr auto operator*() const noexcept
        -> std::string_view {
      const std::string_view rest = Rest();
      if (part_ != kBody) {
        return rest;
      }

      if (const std::string_view escape = WarnEscape(rest.front());
          !escape.empty()) {
        return escape;
      }

      return rest.substr(0, RawRun(rest));
    }

    constexpr auto operator++() noexcept -> iterator& {
      const std::string_view rest = Rest();
      if (part_ != kBody) {
        offset_ += rest.size();
      } else if (!WarnEscape(rest.front()).empty()) {
        ++offset_;
      } else {
        offset_ += RawRun(rest);
      }
      Settle();
      return *this;
    }

    constexpr auto operator++(int) noexcept -> iterator {
      iterator old = *this;
      ++*this;
      return old;
    }

    [[nodiscard]] constexpr auto operator==(const iterator&) const noexcept
        -> bool = default;

   private:
    friend WarnPieces;

    static constexpr std::size_t kBody = 1;
    static constexpr std::size_t kEnd = 3;

    constexpr explicit iterator(const WarnPieces* owner,
                                std::size_t part) noexcept
        : owner_(owner), part_(part) {
      Settle();
    }

    /// @brief Length of the prefix of `rest` that needs no escaping.
    [[nodiscard]] static constexpr auto RawRun(std::string_view rest) noexcept
        -> std::size_t {
      return static_cast<std::size_t>(
          std::ranges::find_if(
              rest, [](char byte) { return !WarnEscape(byte).empty(); }) -
          rest.begin());
    }

    /// @brief The unconsumed remainder of the current piece.
    [[nodiscard]] constexpr auto Rest() const noexcept -> std::string_view {
      return owner_->parts_[part_][index_].substr(offset_);
    }

    /// @brief Skips exhausted and empty pieces, stopping at the end.
    constexpr auto Settle() noexcept -> void {
      while (part_ < kEnd) {
        const std::span<const std::string_view> part = owner_->parts_[part_];
        if (index_ == part.size()) {
          ++part_;
          index_ = 0;
          offset_ = 0;
        } else if (offset_ == part[index_].size()) {
          ++index_;
          offset_ = 0;
        } else {
          return;
        }
      }
    }

    const WarnPieces* owner_ = nullptr;
    std::size_t part_ = kEnd;
    std::size_t index_ = 0;
    std::size_t offset_ = 0;
  };

  [[nodiscard]] constexpr auto begin() const noexcept -> iterator {
    return iterator(this, 0);
  }

  [[nodiscard]] constexpr auto end() const noexcept -> iterator {
    return iterator(this, iterator::kEnd);
  }

 private:
  std::array<std::span<const std::string_view>, 3> parts_;
};

}  // namespace detail

/// @brief warn(3): "<prog>: <fmt>: <strerror(error)>\n" as one write.
template <Writer W, Formattable... Args>
[[nodiscard]] auto Warn(W& writer, std::string_view prog, int error,
                        FormatString<std::type_identity_t<Args>...> fmt,
                        const Args&... args) noexcept
    -> std::expected<void, std::errc> {
  const std::tuple<Formatter<Args>...> formatted(args...);
  const auto body = detail::FormatPieces(fmt.str, formatted);
  const std::array<std::string_view, 2> head{prog, ": "};
  const std::array<std::string_view, 3> tail{": ", os::StrError(error), "\n"};
  return writer.WriteMany(detail::WarnPieces(head, body, tail));
}

/// @brief warnx(3): "<prog>: <fmt>\n" as one write.
template <Writer W, Formattable... Args>
[[nodiscard]] auto Warnx(W& writer, std::string_view prog,
                         FormatString<std::type_identity_t<Args>...> fmt,
                         const Args&... args) noexcept
    -> std::expected<void, std::errc> {
  const std::tuple<Formatter<Args>...> formatted(args...);
  const auto body = detail::FormatPieces(fmt.str, formatted);
  const std::array<std::string_view, 2> head{prog, ": "};
  const std::array<std::string_view, 1> tail{"\n"};
  return writer.WriteMany(detail::WarnPieces(head, body, tail));
}

}  // namespace cutils::io

#endif  // CUTILS_IO_WARN_HPP
