#include "cli.hpp"

#include <expected>
#include <optional>

#include "cutils/io/stderr_writer.hpp"

namespace remmy {

namespace {

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

/// @brief The error code to throw in case of a bad arg parsing.
constexpr int kExitUsage = 64;

/// @brief The usage message to print in case of bad arg parsing.
constexpr std::string_view kUsageMsg =
    "usage: rm [-f | -i] [-dIPRrvWx] file ...\n"
    "       unlink [--] file";

}  // namespace

auto Argv::Parse() noexcept -> std::expected<Cli, UsageError> {
  Cli cli;
  std::size_t index;

  for (index = 0; index < ArgsSpan().size(); ++index) {
    const std::string_view arg = ArgsSpan()[index];
    if (arg.size() < 2 || arg.front() != '-') {
      break;
    }

    if (arg == "--") {
      ++index;
      break;
    }

    for (const char letter : arg.substr(1)) {
      if (!ApplyOption(cli.options, letter)) {
        return std::unexpected(UsageError(letter));
      }
    }
  }

  cli.operands = ArgsSpan().subspan(index);
  if (cli.operands.empty() && !cli.options.force) {
    return std::unexpected(UsageError::Unknown());
  }
  return cli;
}

auto Argv::TryParseOrAbort() noexcept -> std::expected<Cli, int> {
  const auto parsed = Parse();

  if (!parsed) {
    ReportUsage(parsed.error());
    return std::unexpected(kExitUsage);
  }

  // Okay, well we parsed something. Let's abort on flags we don't actually
  // support.
  if (parsed->operands.empty()) {
    return *parsed;
  }

  // TODO(markovejnovic): This is a temporary hack since remmy doesn't support
  //                      all of rm's flags yet.
  return CheckUnsupported(parsed);
}

void Argv::ReportUsage(UsageError err) {
  if (err.IllegalOption()) {
    cutils::io::stderr_writer.PrintLn("{}: illegal option -- {}\n{}",
                                      ProgramName(), *err.IllegalOption(),
                                      kUsageMsg);
  } else {
    cutils::io::stderr_writer.PrintLn("{}", kUsageMsg);
  }
}

auto Argv::CheckUnsupported(Cli cli) noexcept -> std::expected<Cli, int> {
  if (cli.options.interactive) {
    return std::unexpected('i');
  }

  if (cli.options.undelete && !cli.options.recursive) {
    return std::unexpected('W');
  }

  if (cli.options.one_file_system && cli.options.recursive) {
    return std::unexpected('x');
  }

  return cli;
}

}  // namespace remmy
