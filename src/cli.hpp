// See LICENSE in the repository root.

#ifndef REMMY_CLI_HPP
#define REMMY_CLI_HPP

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace remmy {

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

struct Cli;
struct UnlinkCli;

/// @brief The error code to throw in case of a bad arg parsing.
inline constexpr int kExitUsage = 64;

/// @brief A command line to answer with the usage and kExitUsage.
struct UsageError {
  explicit constexpr UsageError(char illegalOption)
      : illegalOption_(illegalOption) {}

  /// @brief An unknown usage error has occurred.
  [[nodiscard]] static constexpr auto Unknown() -> UsageError {
    return UsageError{};
  }

  /// @brief Get the option character that was the root of the problem, if any.
  [[nodiscard]] constexpr auto IllegalOption() const -> std::optional<char> {
    return illegalOption_;
  }

 private:
  explicit constexpr UsageError() : illegalOption_(std::nullopt) {}

  /// @brief The option letter we did not understand, if there was one.
  std::optional<char> illegalOption_;
};

/// @brief Utility for parsing arguments.
///
/// @warn Mutates the argv array. Please do not re-use the given array.
struct Argv {
 public:
  [[nodiscard]] constexpr Argv(int argc, char** argv)
      : span_(argv, static_cast<std::size_t>(argc > 0 ? argc : 0)) {}

  /// @brief Get the arguments as a span. Includes argv[0].
  [[nodiscard]] constexpr auto Span() const -> std::span<char* const> {
    return span_;
  }

  /// @brief Return the span of arguments, excluding argv[0].
  [[nodiscard]] constexpr auto ArgsSpan() const -> std::span<char* const> {
    return Span().empty() ? Span() : Span().subspan(1);
  }

  /// @brief Get how the program was invoked, falling back to "rm".
  [[nodiscard]] constexpr auto InvocationPath() const -> const char* {
    static constexpr const char* kDefaultProgramName = "rm";

    return Span().empty() ? kDefaultProgramName : Span()[0];
  }

  /// @brief The name of the program.
  [[nodiscard]] auto CommandName() const noexcept -> std::string_view;

  /// @brief Try to parse the arguments, returning an error code if parsing
  ///        fails.
  ///
  /// @warn Consumes Argv. Do not re-use it.
  auto TryParseOrAbort() && noexcept
      -> std::expected<std::variant<Cli, UnlinkCli>, int>;

 private:
  /// @brief Report the "usage: ..." message for a given error.
  void ReportUsage(UsageError error) const noexcept;

  /// @brief Whether the application is running in `unlink` mode.
  ///
  /// BSD's `rm` has an unlink compatibility in which it is supposed to naively
  /// pass arguments to `unlink(2)`. This answers the question of whether the
  /// unlink is in that mode or not.
  auto IsUnlinkMode() const noexcept -> bool;

  auto UnlinkOperand() const noexcept -> std::expected<const char*, UsageError>;

  /// @brief Parse the arguments the same way that BSD rm does.
  auto Parse() const noexcept -> std::expected<Cli, UsageError>;

  /// @brief Check whether any of the given exceptions are unsupported and
  ///        abort if so.
  ///
  /// @todo Remove this function altogether as we start support more and more
  ///       options.
  auto CheckUnsupported(Cli options) const noexcept -> std::expected<Cli, int>;

  std::span<char*> span_;
};

/// @brief A parsed command line: the options, and positional arguments.
struct Cli {
  constexpr Cli(remmy::Options options, std::span<char*> operands,
                Argv argv) noexcept
      : options_(options), operands_(operands), argv_(argv) {}

  /// @brief The options given.
  [[nodiscard]] constexpr auto Options() const noexcept
      -> const remmy::Options& {
    return options_;
  }

  /// @brief Arguments considered as operands, ie. all the things that aren't
  ///        options as well as any things that come after `--`.
  [[nodiscard]] constexpr auto Operands() const noexcept
      -> std::span<char* const> {
    return operands_;
  }

  /// @brief See Argv::ProgramName.
  [[nodiscard]] constexpr auto InvocationPath() const -> const char* {
    return argv_.InvocationPath();
  }

  /// @brief See Argv::ExecutableName.
  [[nodiscard]] auto CommandName() const noexcept -> std::string_view {
    return argv_.CommandName();
  }

  [[nodiscard]] constexpr auto ExitsNonZero() const noexcept -> bool {
    return exits_non_zero_;
  }

  /// @brief Check whether any arguments end with `.`, `..` or `/` and print an
  ///        error if so.
  ///
  /// @return A new Cli structure with old data filtered out.
  auto DropUnremovableOperands() noexcept -> void;

 private:
  remmy::Options options_;
  std::span<char*> operands_;

  /// @brief The command line this was parsed from.
  Argv argv_;
  bool exits_non_zero_ = false;
};

struct UnlinkCli {
  constexpr UnlinkCli(const char* operand, Argv argv) noexcept
      : operand_(operand), argv_(argv) {}

  [[nodiscard]] constexpr auto Operand() const noexcept -> const char* {
    return operand_;
  }

  [[nodiscard]] auto CommandName() const noexcept -> std::string_view {
    return argv_.CommandName();
  }

 private:
  const char* operand_;
  Argv argv_;
};

}  // namespace remmy

#endif
