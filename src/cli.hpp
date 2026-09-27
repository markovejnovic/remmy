// See LICENSE in the repository root.

#ifndef REMMY_CLI_HPP
#define REMMY_CLI_HPP

#include <cstddef>
#include <expected>
#include <optional>
#include <span>

namespace remmy {

namespace detail {

static constexpr const char* kDefaultProgramName = "rm";

}  // namespace detail

/// @brief The options of BSD rm(1): getopt(3) with "dfiIPRrvWx".
struct Options {
  bool dir = false;              ///< -d: remove empty directories as well.
  bool force = false;            ///< -f: never prompt, ignore missing files.
  bool interactive = false;      ///< -i: prompt before each removal.
  bool prompt_once = false;      ///< -I: prompt once before a large removal.
  bool overwrite = false;        ///< -P: overwrite regular files first.
  bool recursive = false;        ///< -R, -r: remove file hierarchies.
  bool verbose = false;          ///< -v: print each path once removed.
  bool undelete = false;         ///< -W: undelete instead of removing.
  bool one_file_system = false;  ///< -x: stay on the operands' devices.
};

/// @brief A parsed command line: the options and what follows them.
struct Cli {
  Options options;

  /// @brief Arguments considered as operands, ie. all the things that aren't
  ///        options as well as any things that come after `--`.
  std::span<const char*> operands;
};

/// @brief A command line to answer with the usage and kExitUsage.
struct UsageError {
  explicit constexpr UsageError(char illegalOption)
      : illegalOption_(illegalOption) {}

  /// @brief An unknown usage error has occurred.
  [[nodiscard]] static constexpr auto Unknown() -> UsageError {
    return UsageError{};
  }

  /// @brief Get the option character that was the root of the problem, if any.
  [[nodiscard]] constexpr auto IllegalOption() -> std::optional<char> {
    return illegalOption_;
  }

 private:
  explicit constexpr UsageError() : illegalOption_(std::nullopt) {}

  /// @brief The option letter we did not understand, if there was one.
  std::optional<char> illegalOption_;
};

struct Argv {
 public:
  [[nodiscard]] constexpr Argv(std::size_t argc, const char** argv)
      : span_(argv, static_cast<std::size_t>(argc > 0 ? argc : 0)) {}

  /// @brief Get the arguments as a span. Includes argv[0].
  [[nodiscard]] constexpr auto Span() -> std::span<const char*> {
    return span_;
  }

  /// @brief Return the span of arguments, excluding argv[0].
  [[nodiscard]] constexpr auto ArgsSpan() -> std::span<const char*> {
    return Span().empty() ? Span() : Span().subspan(1);
  }

  /// @brief Get the program name.
  [[nodiscard]] constexpr auto ProgramName() -> const char* {
    return Span().empty() ? detail::kDefaultProgramName : Span()[0];
  }

  /// @brief Parse the arguments the same way that BSD rm does.
  auto Parse() noexcept -> std::expected<Cli, UsageError>;

  /// @brief Try to parse the arguments, returning an error code if parsing
  ///        fails.
  auto TryParseOrAbort() noexcept -> std::expected<Cli, int>;

 private:
  /// @brief Report the "usage: ..." message for a given error.
  auto ReportUsage(UsageError error);

  /// @brief Check whether any of the given exceptions are unsupported and
  ///        abort if so.
  ///
  /// @todo Remove this function altogether as we start support more and more
  ///       options.
  auto ReportIfUnsupported(Cli options) -> std::expected<Cli, int>;

  std::span<const char*> span_;
};

}  // namespace remmy

#endif
