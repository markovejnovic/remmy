// See LICENSE in the repository root.

#include "prompt.hpp"

#include <locale.h>
#include <sys/stat.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

#include "cutils/io/print.hpp"
#include "cutils/os/file_flags.hpp"
#include "cutils/os/mode_string.hpp"
#include "cutils/os/os.hpp"
#include "cutils/os/owner_names.hpp"

namespace remmy {

void RemovePrompt::Render(PromptWriter& out) const noexcept {
  std::ignore = cutils::io::Print(out, "remove {}? ", path);
}

void ExaminePrompt::Render(PromptWriter& out) const noexcept {
  std::ignore = cutils::io::Print(out, "examine files in directory {}? ", path);
}

void OverridePrompt::Render(PromptWriter& out) const noexcept {
  // rm prints strmode's text past the type letter, a space only if
  // strmode left none, then any flags with a space after them.
  const cutils::os::ModeString mode(info.mode);
  const std::string_view text = mode.View();
  const std::string flags = cutils::os::FlagNames(info.flags);
  std::ignore = cutils::io::Print(
      out, "override {}{}{}/{} {}{}for {}? ", text.substr(1),
      std::string_view{text.back() == ' ' ? "" : " "},
      cutils::os::UserName(info.uid), cutils::os::GroupName(info.gid), flags,
      std::string_view{flags.empty() ? "" : " "}, path);
}

void BulkPrompt::Render(PromptWriter& out) const noexcept {
  if (recursive && dirs > 0) {
    std::ignore = cutils::io::Print(out, "recursively remove");
    if (dirs == 1) {
      std::ignore = cutils::io::Print(out, " {}", dir_name);
    } else {
      std::ignore = cutils::io::Print(out, " {} dirs", dirs);
    }
    if (files == 1) {
      std::ignore = cutils::io::Print(out, " and 1 file");
    } else if (files > 1) {
      std::ignore = cutils::io::Print(out, " and {} files", files);
    }
  } else {
    std::ignore = cutils::io::Print(out, "remove {} files", dirs + files);
  }
  std::ignore = cutils::io::Print(out, "? ");
}

auto BulkPromptFor(std::span<char* const> operands, bool recursive) noexcept
    -> std::optional<BulkPrompt> {
  static constexpr std::size_t kMaxSilentOperands = 3;

  BulkPrompt prompt{
      .dirs = 0, .files = 0, .dir_name = {}, .recursive = recursive};
  for (const char* path : operands) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      continue;
    }
    if (S_ISDIR(path_stat.st_mode)) {
      ++prompt.dirs;
      prompt.dir_name = path;
    } else {
      ++prompt.files;
    }
  }

  if (!(recursive && prompt.dirs > 0) &&
      prompt.dirs + prompt.files <= kMaxSilentOperands) {
    return std::nullopt;
  }
  return prompt;
}

AnswerLocale::AnswerLocale() noexcept
    : locale_(::newlocale(LC_ALL_MASK, "", nullptr)) {}

AnswerLocale::~AnswerLocale() {
  if (locale_ != nullptr) {
    ::freelocale(locale_);
  }
}

auto AnswerLocale::IsYes(char first) const noexcept -> bool {
  const std::array<char, 2> response{first, '\0'};
  locale_t previous =
      ::uselocale(locale_ != nullptr ? locale_ : LC_GLOBAL_LOCALE);
  const int matched = ::rpmatch(response.data());
  ::uselocale(previous);
  return matched == 1;
}

auto Console::ReadFirstByteRpMatch() noexcept -> Answer {
  const std::optional<char> first = ReadByte();
  if (!first) {
    return Answer::kNo;
  }
  if (*first != '\n') {
    SkipLine();
  }
  if (!locale_) {
    locale_.emplace();
  }
  return locale_->IsYes(*first) ? Answer::kYes : Answer::kNo;
}

auto Console::ReadByte() noexcept -> std::optional<char> {
  if (at_end_) {
    return std::nullopt;
  }
  const std::optional<char> byte = in_.ReadByte();
  at_end_ = !byte;
  return byte;
}

auto Console::SkipLine() noexcept -> bool {
  at_end_ = at_end_ || !in_.SkipPast('\n');
  return !at_end_;
}

auto Console::ReadStrictYesNo() noexcept -> std::optional<Answer> {
  const std::optional<char> first = ReadByte();
  const bool line_ended = first == '\n' || (first && SkipLine());

  if (first == 'y' || first == 'Y') {
    return Answer::kYes;
  }
  if (first == 'n' || first == 'N' || !line_ended) {
    return Answer::kNo;
  }
  return std::nullopt;
}

}  // namespace remmy
