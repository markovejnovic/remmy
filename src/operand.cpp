// See LICENSE in the repository root.

#include "operand.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>

#include "cutils/os/os.hpp"

namespace remmy {

auto Operand::ThroughSymlink() const -> bool {
  std::string_view path = Path();
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  std::string prefix;
  for (std::size_t slash = path.find('/', 1); slash != std::string_view::npos;
       slash = path.find('/', slash + 1)) {
    if (path[slash - 1] == '/') {
      continue;
    }
    prefix.assign(path.substr(0, slash));
    struct stat prefix_stat;
    if (cutils::os::lstat(prefix.c_str(), &prefix_stat) == 0 &&
        S_ISLNK(prefix_stat.st_mode)) {
      return true;
    }
  }
  return false;
}

auto Operand::FollowsTrailingLink() const -> bool {
  std::string_view entry = Path();
  while (entry.size() > 1 && entry.back() == '/') {
    entry.remove_suffix(1);
  }
  if (entry.size() == Path().size()) {
    return false;
  }
  const std::string terminated(entry);
  struct stat entry_stat;
  return cutils::os::lstat(terminated.c_str(), &entry_stat) == 0 &&
         !S_ISDIR(entry_stat.st_mode);
}

auto Resolve(Operand operand) noexcept -> ResolvedOperand {
  struct stat path_stat;
  if (cutils::os::lstat(operand.CStr(), &path_stat) != 0) {
    return MissingOperand{.operand = operand, .error = errno};
  }
  if (!S_ISDIR(path_stat.st_mode)) {
    return FileOperand{.operand = operand};
  }
  return DirectoryOperand{.operand = operand, .device = path_stat.st_dev};
}

}  // namespace remmy
