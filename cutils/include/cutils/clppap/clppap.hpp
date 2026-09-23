// See LICENSE in the repository root.

/// @brief Reflection-based CLI parsing argument.
///
/// I have gotten a little distracted with this library, but I thought it
/// would be kind of neat to play around with reflection.
///
/// This little utility allows you to write clap-style code. It's
/// allocation-free, at the cost of a repeated scan on positional
/// arguments. That's a worth-while tradeoff since things are already in
/// the cache line.
#ifndef CUTILS_CLPPAP_CLPPAP_HPP
#define CUTILS_CLPPAP_CLPPAP_HPP

#include <cutils/clppap/detail/argument_set.hpp>
#include <cutils/clppap/detail/schema.hpp>

namespace cpplap {

inline constexpr PositionalTag Positional{};

inline constexpr HelpFlagTag HelpFlag{};

template <typename T>
[[nodiscard]] constexpr auto HelpText() noexcept -> std::string_view {
  return detail::kHelpText<T>;
}

template <typename T>
[[nodiscard]] auto Parse(int argc, const char* const* argv) noexcept
    -> std::expected<T, ParseError> {
  constexpr auto& schema = detail::kSchema<T>;
  constexpr T defaults{};

  const auto arguments = detail::ArgumentSet<detail::kFlagCount<T>>::Scan(
      argc, argv, schema.options,
      schema.has_positionals ? detail::PositionalPolicy::Allow
                             : detail::PositionalPolicy::Reject);
  if (!arguments) {
    return std::unexpected(arguments.error());
  }

  T result = defaults;
  std::size_t index = 0;
  template for (constexpr auto member : detail::kMembers<T>) {
    if constexpr (std::meta::type_of(member) == (^^bool)) {
      if (arguments->Flag(index++)) {
        result.[:member:] = true;
      }
    } else if constexpr (std::meta::type_of(member) == (^^PositionalIterator)) {
      result.[:member:] = arguments->Positionals();
    }
  }
  return result;
}

}  // namespace cpplap

#endif  // CUTILS_CLPPAP_CLPPAP_HPP
