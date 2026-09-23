// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_POSITIONAL_ITERATOR_HPP
#define CUTILS_CLPPAP_POSITIONAL_ITERATOR_HPP

#include <cstddef>
#include <cutils/clppap/detail/arg_classifier.hpp>
#include <expected>
#include <iterator>

namespace cpplap {

class PositionalIterator {
 public:
  using value_type = const char*;
  using difference_type = std::ptrdiff_t;
  using iterator_concept = std::input_iterator_tag;
  using iterator_category = std::input_iterator_tag;

  constexpr PositionalIterator(std::span<const char* const> arguments,
                               std::span<const Option> options) noexcept
      : classifier_(arguments, options) {
    Seek();
  }

  [[nodiscard]] constexpr auto operator*() const noexcept -> value_type {
    return (*classifier_)->argument;
  }

  constexpr auto operator++() noexcept -> PositionalIterator& {
    if (classifier_ != std::default_sentinel) {
      ++classifier_;
      Seek();
    }
    return *this;
  }

  constexpr void operator++(int) noexcept { ++*this; }

  [[nodiscard]] constexpr auto operator==(
      std::default_sentinel_t) const noexcept -> bool {
    return classifier_ == std::default_sentinel;
  }

  [[nodiscard]] constexpr auto begin() const noexcept -> PositionalIterator {
    return *this;
  }

  [[nodiscard]] constexpr auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

 private:
  template <typename T>
  friend auto Parse(int argc, const char* const* argv) noexcept
      -> std::expected<T, ParseError>;

  constexpr PositionalIterator() noexcept = default;

  constexpr void Seek() noexcept {
    while (classifier_ != std::default_sentinel &&
           (*classifier_)->kind != detail::TokenKind::Positional) {
      ++classifier_;
    }
  }

  detail::ArgClassifier classifier_{{}, {}};
};

}  // namespace cpplap

#endif  // CUTILS_CLPPAP_POSITIONAL_ITERATOR_HPP
