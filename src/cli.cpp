#include "cli.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include "cutils/io/print.hpp"
#include "cutils/io/stderr_writer.hpp"
#include "cutils/os/os.hpp"

namespace remmy {

namespace {

/// @brief The error code to throw in case of a bad arg parsing.
constexpr int kExitUsage = 64;

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

constexpr int kExitUnsupported = 1;

/// @brief The usage message to print in case of bad arg parsing.
constexpr std::string_view kUsageMsg =
    "usage: rm [-f | -i] [-dIPRrvWx] file ...\n"
    "       unlink [--] file";

/// @brief Whether the operand's last component is `.` or `..`.
constexpr auto IsDotOrDotDotOperand(std::string_view path) noexcept -> bool {
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }

  const std::size_t slash = path.rfind('/');
  const std::string_view last =
      slash == std::string_view::npos ? path : path.substr(slash + 1);

  return last == "." || last == "..";
}

}  // namespace

/// @brief Whether rm runs as unlink(1).
auto Argv::IsUnlinkMode() const noexcept -> bool {
  const std::string_view argv0 = InvocationPath();
  const std::size_t slash = argv0.rfind('/');

  return (slash == std::string_view::npos ? argv0 : argv0.substr(slash + 1)) ==
         "unlink";
}

/// @brief The one operand of an unlink(1) command line, or nullptr when it is
///        a usage error.
///
/// unlink(1) parses no options: it takes exactly one operand, which a single
/// leading "--" may precede. So "-f" alone is a file's name, "--" alone is one
/// too, and "-f file" or "a b" are usage errors.
auto Argv::ParseUnlinkOperand() const noexcept
    -> std::expected<const char*, UsageError> {
  const std::span<char* const> args = ArgsSpan();
  if (args.size() == 1) {
    return args.front();
  }
  if (args.size() == 2 && std::string_view(args.front()) == "--") {
    return args.back();
  }
  return std::unexpected(UsageError::Unknown());
}

auto Argv::Parse() const noexcept -> std::expected<Cli, UsageError> {
  Options options;
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
      if (!ApplyOption(options, letter)) {
        return std::unexpected(UsageError(letter));
      }
    }
  }

  const std::span<char*> operands = span_.last(ArgsSpan().size() - index);
  if (operands.empty() && !options.force) {
    return std::unexpected(UsageError::Unknown());
  }
  return Cli(options, operands, *this);
}

auto Argv::CommandName() const noexcept -> std::string_view {
  if (const std::string_view name = cutils::os::GetProgName(); !name.empty()) {
    return name;
  }

  const std::string_view argv0 = InvocationPath();
  const std::string_view base = argv0.substr(argv0.rfind('/') + 1);

  return base.empty() ? "rm" : base;
}

auto Argv::TryParseOrAbort() && noexcept
    -> std::expected<std::variant<Cli, UnlinkCli>, int> {
  if (IsUnlinkMode()) {
    const auto operand = ParseUnlinkOperand();
    if (!operand) {
      ReportUsage(operand.error());
      return std::unexpected(kExitUsage);
    }

    return UnlinkCli(*operand, *this);
  }

  auto parsed = Parse();

  if (!parsed) {
    ReportUsage(parsed.error());
    return std::unexpected(kExitUsage);
  }

  parsed->DropUnremovableOperands();

  // Okay, well we parsed something. Let's abort on flags we don't actually
  // support.
  if (parsed->Operands().empty()) {
    return *std::move(parsed);
  }

  // TODO(markovejnovic): This is a temporary hack since remmy doesn't support
  //                      all of rm's flags yet.
  return CheckUnsupported(*std::move(parsed));
}

void Argv::ReportUsage(UsageError err) const noexcept {
  if (err.IllegalOption()) {
    std::ignore = cutils::io::PrintLn(
        cutils::io::stderr_writer, "{}: illegal option -- {}\n{}",
        InvocationPath(), *err.IllegalOption(), kUsageMsg);
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
                            InvocationPath(), option);
    return std::unexpected(kExitUnsupported);
  };

  if (cli.Options().interactive) {
    return refuse('i');
  }
  if (cli.Options().undelete && !cli.Options().recursive) {
    return refuse('W');
  }
  if (cli.Options().one_file_system && cli.Options().recursive) {
    return refuse('x');
  }

  return cli;
}

auto Cli::DropUnremovableOperands() noexcept -> void {
  const auto is_slash = [](std::string_view path) { return path == "/"; };
  const bool dot = std::ranges::any_of(operands_, IsDotOrDotDotOperand);
  const bool slash = std::ranges::any_of(operands_, is_slash);
  const auto dropped =
      std::ranges::remove_if(operands_, [&](std::string_view path) {
        return IsDotOrDotDotOperand(path) || is_slash(path);
      });
  operands_ = operands_.first(operands_.size() - dropped.size());

  if (dot) {
    std::ignore = cutils::io::PrintLn(cutils::io::stderr_writer,
                                      "{}: \".\" and \"..\" may not be removed",
                                      CommandName());
  }
  if (slash) {
    std::ignore =
        cutils::io::PrintLn(cutils::io::stderr_writer,
                            "{}: \"/\" may not be removed", CommandName());
  }
  has_dropped_operands_ |= dot || slash;
}

}  // namespace remmy
