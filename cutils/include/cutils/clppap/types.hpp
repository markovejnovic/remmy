// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_TYPES_HPP
#define CUTILS_CLPPAP_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cpplap {

struct Option {
  std::string_view short_name;
  std::string_view long_name;
  std::string_view help;
};

enum class ErrorCode : std::uint8_t {
  InvalidArgv,
  NullArgument,
  UnknownOption,
  UnexpectedPositional
};

struct ParseError {
  constexpr ParseError(ErrorCode error_code, std::size_t index,
                       std::string_view failing_token) noexcept
      : code(error_code), argument_index(index), token(failing_token) {}

  ErrorCode code;
  std::size_t argument_index;
  std::string_view token;
};

}  // namespace cpplap

#endif  // CUTILS_CLPPAP_TYPES_HPP
