// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_REPORT_HPP
#define CUTILS_CLPPAP_REPORT_HPP

#include <cstdio>
#include <cutils/clppap/clppap.hpp>
#include <cutils/clppap/detail/schema.hpp>
#include <expected>
#include <format>
#include <print>
#include <string_view>

template <>
struct std::formatter<cpplap::ParseError> {
  static constexpr auto parse(std::format_parse_context& context) {
    return context.begin();
  }

  static auto format(const cpplap::ParseError& error,
                     std::format_context& context) {
    switch (error.code) {
      case cpplap::ErrorCode::InvalidArgv:
        return std::format_to(context.out(), "malformed argument vector");
      case cpplap::ErrorCode::NullArgument:
        return std::format_to(context.out(), "argument {} is a null pointer",
                              error.argument_index);
      case cpplap::ErrorCode::UnknownOption:
        return std::format_to(context.out(), "unknown option {}: '{}'",
                              error.argument_index, error.token);
      case cpplap::ErrorCode::UnexpectedPositional:
        return std::format_to(context.out(), "unexpected argument {}: '{}'",
                              error.argument_index, error.token);
    }
    return context.out();
  }
};

namespace cpplap {

namespace detail {

[[nodiscard]] constexpr auto ProgramName(int argc,
                                         const char* const* argv) noexcept
    -> std::string_view {
  if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
    return {};
  }
  const std::string_view path{argv[0]};
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

template <typename T>
[[nodiscard]] auto HelpRequested(const T& parsed) noexcept -> bool {
  constexpr auto& schema = kSchema<T>;
  if constexpr (!schema.has_help) {
    return false;
  } else {
    std::size_t index = 0;
    template for (constexpr auto member : kMembers<T>) {
      if constexpr (std::meta::type_of(member) == (^^bool)) {
        if (index++ == schema.help_index) {
          return parsed.[:member:];
        }
      }
    }
    return false;
  }
}

}  // namespace detail

template <typename T>
[[nodiscard]] auto ParseOrReport(int argc, const char* const* argv,
                                 std::FILE* out = stdout,
                                 std::FILE* err = stderr)
    -> std::expected<T, int> {
  const auto parsed = Parse<T>(argc, argv);
  if (!parsed) {
    if (const auto program = detail::ProgramName(argc, argv); program.empty()) {
      std::println(err, "{}", parsed.error());
    } else {
      std::println(err, "{}: {}", program, parsed.error());
    }
    return std::unexpected(1);
  }
  if (detail::HelpRequested(*parsed)) {
    std::print(out, "{}", HelpText<T>());
    return std::unexpected(0);
  }
  return *parsed;
}

}  // namespace cpplap

#endif  // CUTILS_CLPPAP_REPORT_HPP
