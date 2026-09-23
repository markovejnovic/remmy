// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_DETAIL_ARG_CLASSIFIER_HPP
#define CUTILS_CLPPAP_DETAIL_ARG_CLASSIFIER_HPP

#include <cstddef>
#include <cstdint>
#include <cutils/clppap/types.hpp>
#include <expected>
#include <iterator>
#include <ranges>
#include <span>
#include <string_view>

namespace cpplap::detail {

enum class TokenKind : std::uint8_t { Flag, Positional, Separator };

struct Token {
  TokenKind kind;
  std::size_t option_index = 0;
  const char* argument = nullptr;
  std::size_t argument_index = 0;
};

struct ArgClassifier : std::ranges::view_interface<ArgClassifier> {
  using value_type = std::expected<Token, ParseError>;
  using difference_type = std::ptrdiff_t;
  using iterator_concept = std::input_iterator_tag;
  using iterator_category = std::input_iterator_tag;

  constexpr ArgClassifier(std::span<const char* const> arguments,
                          std::span<const Option> options) noexcept
      : arguments_(arguments), options_(options) {}

  [[nodiscard]] constexpr auto operator*() const noexcept -> value_type {
    return Classify();
  }

  constexpr auto operator++() noexcept -> ArgClassifier& {
    if (position_ < arguments_.size()) {
      if (!options_ended_) {
        const auto token = Classify();
        options_ended_ = token && token->kind == TokenKind::Separator;
      }
      ++position_;
    }
    return *this;
  }

  constexpr void operator++(int) noexcept { ++*this; }

  [[nodiscard]] constexpr auto operator==(
      std::default_sentinel_t) const noexcept -> bool {
    return position_ == arguments_.size();
  }

  [[nodiscard]] constexpr auto begin() const noexcept -> ArgClassifier {
    return *this;
  }

  [[nodiscard]] constexpr auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

 private:
  [[nodiscard]] constexpr auto Classify() const noexcept -> value_type {
    Token token{.kind = TokenKind::Positional,
                .argument = arguments_[position_],
                .argument_index = position_ + 1};

    if (token.argument == nullptr) {
      return std::unexpected(
          ParseError{ErrorCode::NullArgument, token.argument_index, {}});
    }

    const std::string_view word{token.argument};
    if (options_ended_ || word.empty() || word == "-" || word.front() != '-') {
      return token;
    }

    if (word == "--") {
      token.kind = TokenKind::Separator;
      return token;
    }

    for (std::size_t i = 0; i < options_.size(); ++i) {
      if (word == options_[i].short_name || word == options_[i].long_name) {
        token.kind = TokenKind::Flag;
        token.option_index = i;
        return token;
      }
    }

    return std::unexpected(
        ParseError{ErrorCode::UnknownOption, token.argument_index, word});
  }

  std::span<const char* const> arguments_;
  std::span<const Option> options_;
  std::size_t position_ = 0;
  bool options_ended_ = false;
};

}  // namespace cpplap::detail

#endif  // CUTILS_CLPPAP_DETAIL_ARG_CLASSIFIER_HPP
