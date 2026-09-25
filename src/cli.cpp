#include "cli.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>
#include <tuple>

#include "cutils/io/print.hpp"
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

constexpr int kExitUnsupported = 1;

/// @brief The usage message to print in case of bad arg parsing.
constexpr std::string_view kUsageMsg =
    "usage: rm [-f | -i] [-dIPRrvWx] file ...\n"
    "       unlink [--] file";

}  // namespace

auto Argv::Parse() const noexcept -> std::expected<Cli, UsageError> {
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

auto Argv::TryParseOrAbort() const noexcept -> std::expected<Cli, int> {
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
  return CheckUnsupported(*parsed);
}

void Argv::ReportUsage(UsageError err) const noexcept {
  if (err.IllegalOption()) {
    std::ignore = cutils::io::PrintLn(
        cutils::io::stderr_writer, "{}: illegal option -- {}\n{}",
        ProgramName(), *err.IllegalOption(), kUsageMsg);
  } else {
    std::ignore =
        cutils::io::PrintLn(cutils::io::stderr_writer, "{}", kUsageMsg);
  }
}

auto Argv::CheckUnsupported(Cli cli) const noexcept -> std::expected<Cli, int> {
  const auto refuse = [this](char option) -> std::expected<Cli, int> {
    std::ignore =
        cutils::io::PrintLn(cutils::io::stderr_writer,
                            "{}: -{}: not supported yet; nothing was removed",
                            ProgramName(), option);
    return std::unexpected(kExitUnsupported);
  };

  if (cli.options.undelete && !cli.options.recursive) {
    return refuse('W');
  }
  if (cli.options.one_file_system && cli.options.recursive) {
    return refuse('x');
  }

  return cli;
}

}  // namespace remmy
