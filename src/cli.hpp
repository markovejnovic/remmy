// See LICENSE in the repository root.

#ifndef REMMY_CLI_HPP
#define REMMY_CLI_HPP

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace remmy {

/// @brief Exit status of a command line rm(1) rejects (sysexits' EX_USAGE).
inline constexpr int kExitUsage = 64;

/// @brief The usage text BSD rm prints, byte for byte, whatever its name.
inline constexpr std::string_view kUsage =
    "usage: rm [-f | -i] [-dIPRrvWx] file ...\n"
    "       unlink [--] file\n";

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
  std::span<char* const> operands;
};

/// @brief A command line to answer with the usage and kExitUsage.
struct UsageError {
  /// @brief The option letter getopt(3) did not know, or '\0' when the only
  ///        problem is that there are no operands.
  char illegal_option = '\0';
};

/// @brief Applies one option letter, false when it is not one of rm's.
constexpr auto ApplyOption(Options& options, char letter) noexcept -> bool {
  switch (letter) {
    case 'd':
      options.dir = true;
      return true;
    // -f and -i override each other: the last one wins.
    case 'f':
      options.force = true;
      options.interactive = false;
      return true;
    case 'i':
      options.interactive = true;
      options.force = false;
      return true;
    case 'I':
      options.prompt_once = true;
      return true;
    case 'P':
      options.overwrite = true;
      return true;
    case 'R':
    case 'r':
      options.recursive = true;
      return true;
    case 'v':
      options.verbose = true;
      return true;
    case 'W':
      options.undelete = true;
      return true;
    case 'x':
      options.one_file_system = true;
      return true;
    default:
      return false;
  }
}

/// @brief The first parsed option remmy cannot honour yet and would otherwise
///        remove too much under, or '\0' when there is none.
///
/// Such a command line must be refused before anything is touched: an
/// effective -i (so not one a later -f overrode) asks before every removal,
/// -W without -r undeletes rather than removes, and -x with -r keeps the walk
/// on each operand's device. -W with -r only adds whiteouts to the walk and -x
/// without -r changes nothing, so both are safe to ignore there. -I depends on
/// the operands; see the caller. -d, -P and -v never remove more than remmy
/// already does without them.
constexpr auto UnsupportedOption(const Options& options) noexcept -> char {
  if (options.interactive) {
    return 'i';
  }
  if (options.undelete && !options.recursive) {
    return 'W';
  }
  if (options.one_file_system && options.recursive) {
    return 'x';
  }
  return '\0';
}

/// @brief Parses the arguments after argv[0] the way BSD rm does.
///
/// Options may be clustered and repeated. Parsing stops at the first operand,
/// at a lone "-" (an operand) and after "--"; nothing is permuted, so options
/// after an operand are operands too. Any other letter, '-' included (so every
/// "--long" option), is illegal. With no operands rm prints its usage, unless
/// -f is in effect, which makes that a silent success.
constexpr auto ParseCli(std::span<char* const> args) noexcept
    -> std::expected<Cli, UsageError> {
  Cli cli;
  std::size_t index = 0;
  for (; index < args.size(); ++index) {
    const std::string_view arg = args[index];
    if (arg.size() < 2 || arg.front() != '-') {
      break;
    }
    if (arg == "--") {
      ++index;
      break;
    }
    for (const char letter : arg.substr(1)) {
      if (!ApplyOption(cli.options, letter)) {
        return std::unexpected(UsageError{.illegal_option = letter});
      }
    }
  }

  cli.operands = args.subspan(index);
  if (cli.operands.empty() && !cli.options.force) {
    return std::unexpected(UsageError{});
  }
  return cli;
}

}  // namespace remmy

#endif
