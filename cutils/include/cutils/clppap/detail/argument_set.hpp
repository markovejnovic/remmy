// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_DETAIL_ARGUMENT_SET_HPP
#define CUTILS_CLPPAP_DETAIL_ARGUMENT_SET_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cutils/clppap/detail/arg_classifier.hpp>
#include <cutils/clppap/positional_iterator.hpp>
#include <expected>
#include <span>

namespace cpplap::detail {

enum class PositionalPolicy : std::uint8_t { Reject, Allow };

template <std::size_t N>
class ArgumentSet {
 public:
  [[nodiscard]] static constexpr auto Scan(
      std::span<const char* const> arguments,
      std::span<const Option, N> options, PositionalPolicy policy) noexcept
      -> std::expected<ArgumentSet, ParseError> {
    ArgumentSet out{arguments, options};
    for (const auto classified : ArgClassifier{arguments, options}) {
      if (!classified) {
        return std::unexpected(classified.error());
      }
      const auto& token = *classified;
      switch (token.kind) {
        case TokenKind::Flag:
          out.seen_[token.option_index] = true;
          break;
        case TokenKind::Separator:
          break;
        case TokenKind::Positional:
          if (policy == PositionalPolicy::Reject) {
            return std::unexpected(ParseError{ErrorCode::UnexpectedPositional,
                                              token.argument_index,
                                              token.argument});
          }
          break;
      }
    }
    return out;
  }

  [[nodiscard]] static constexpr auto Scan(int argc, const char* const* argv,
                                           std::span<const Option, N> options,
                                           PositionalPolicy policy) noexcept
      -> std::expected<ArgumentSet, ParseError> {
    if (argc < 0 || (argc > 0 && (argv == nullptr || argv[0] == nullptr))) {
      return std::unexpected(ParseError{ErrorCode::InvalidArgv, 0, {}});
    }
    return Scan(
        argc > 0
            ? std::span<const char* const>{argv + 1,
                                           static_cast<std::size_t>(argc - 1)}
            : std::span<const char* const>{},
        options, policy);
  }

  [[nodiscard]] constexpr auto Flag(std::size_t index) const noexcept -> bool {
    return seen_[index];
  }

  [[nodiscard]] constexpr auto Positionals() const noexcept
      -> PositionalIterator {
    return PositionalIterator{arguments_, options_};
  }

 private:
  constexpr ArgumentSet(std::span<const char* const> arguments,
                        std::span<const Option> options) noexcept
      : arguments_(arguments), options_(options) {}

  std::span<const char* const> arguments_;
  std::span<const Option> options_;
  std::array<bool, N> seen_{};
};

}  // namespace cpplap::detail

#endif  // CUTILS_CLPPAP_DETAIL_ARGUMENT_SET_HPP
