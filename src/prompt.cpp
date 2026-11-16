// See LICENSE in the repository root.

#include "prompt.hpp"

#include <sys/stat.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "cutils/io/print.hpp"
#include "cutils/os/os.hpp"

namespace remmy {

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

auto Console::ReadStrictYesNo() noexcept -> std::optional<Answer> {
  const std::optional<char> first = in_.ReadByte();
  const bool line_ended = first == '\n' || (first && in_.SkipPast('\n'));

  if (first == 'y' || first == 'Y') {
    return Answer::kYes;
  }
  if (first == 'n' || first == 'N' || !line_ended) {
    return Answer::kNo;
  }
  return std::nullopt;
}

}  // namespace remmy
