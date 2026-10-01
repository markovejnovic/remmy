// See LICENSE in the repository root.

#include "dir_node.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "cutils/os/os.hpp"
#include "walk.hpp"

namespace remmy {

namespace {

/// @brief openat relative to `base`, or open when it is AT_FDCWD.
auto OpenAt(int base, const char* name, int flags) noexcept
    -> std::expected<cutils::os::Fd, cutils::os::OpenError> {
  if (base == AT_FDCWD) {
    return cutils::os::open(name, flags);
  }
  return cutils::os::Fd::Open([&] { return ::openat(base, name, flags); });
}

/// @brief Open `path`, even when it is PATH_MAX bytes or longer.
///
/// Returns why it could not be opened on failure.
auto OpenLong(int base, std::string_view path, int flags) noexcept
    -> std::expected<cutils::os::Fd, cutils::os::OpenError> {
  std::array<char, PATH_MAX> piece;
  cutils::os::Fd dir;
  while (path.size() >= PATH_MAX) {
    const std::size_t cut = path.rfind('/', PATH_MAX - 1);
    if (cut == std::string_view::npos || cut == 0) {
      return std::unexpected(cutils::os::OpenError{
          .code = std::errc::filename_too_long, .retryable = false});
    }
    std::copy_n(path.begin(), cut, piece.begin());
    piece[cut] = '\0';
    auto next = OpenAt(base, piece.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!next) {
      return std::unexpected(next.error());
    }
    dir = *std::move(next);
    base = dir.get();
    // The rest is relative to `dir`, even after a doubled slash.
    path.remove_prefix(cut);
    while (!path.empty() && path.front() == '/') {
      path.remove_prefix(1);
    }
  }
  std::copy_n(path.begin(), path.size(), piece.begin());
  piece[path.size()] = '\0';
  return OpenAt(base, piece.data(), flags);
}

}  // namespace

auto DirNode::Parents() const noexcept -> ParentChain {
  return ParentChain{this};
}

auto DirNode::ParentsMut() noexcept -> MutableParentChain {
  return MutableParentChain{this};
}

auto DirNode::PathInto(std::string& out, std::string_view root) const noexcept
    -> const char* {
  // The name a node contributes: its own, or `root` in place of the root's.
  const auto name_of = [root](const DirNode* t) -> std::string_view {
    return t->parent_ == nullptr && !root.empty() ? root : t->name_;
  };

  // I want to avoid resizing here too much, so first we count the total
  // number of bytes we'd need in the directory tree.
  const std::size_t total = std::ranges::fold_left(
      Parents(), std::size_t{0}, [&name_of](std::size_t acc, const DirNode* t) {
        return acc + name_of(t).size() +
               static_cast<std::size_t>(t->parent_ != nullptr);
      });

  // This is so janky, but it is the price you pay to avoid allocations.
  //
  // We make _another_ scan through the directory nodes (hopefully they're
  // all in-cache), and we write data in reverse order.
  const auto write = [this, &name_of](char* data, std::size_t size) {
    char* cursor = data + size;
    for (const DirNode* t : Parents()) {
      const std::string_view name = name_of(t);
      cursor -= name.size();
      std::copy_n(name.data(), name.size(), cursor);
      if (t->parent_ != nullptr) {
        *--cursor = '/';
      }
    }
    return size;
  };
  out.resize_and_overwrite(total, write);
  return out.c_str();
}

auto DirNode::Open(std::string& scratch) noexcept
    -> std::expected<void, cutils::os::OpenError> {
  if (fd_.IsOpen()) {
    return {};
  }

  const KernelPath at = walk_.PathOf(*this, scratch);
  auto opened = OpenLong(at.base, at.path,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!opened) {
    return std::unexpected(opened.error());
  }
  fd_ = *std::move(opened);
  return {};
}

auto DirNode::RemoveEmpty(std::string& scratch) const noexcept -> int {
  const KernelPath at = walk_.PathOf(*this, scratch);
  const char* const c_path = at.path;
  const std::string_view path = scratch;
  if (path.size() < PATH_MAX) {
    return at.base == AT_FDCWD ? cutils::os::rmdir(c_path)
                               : ::unlinkat(at.base, c_path, AT_REMOVEDIR);
  }

  // Too long for rmdir: remove it relative to its parent instead.
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    errno = ENAMETOOLONG;
    return -1;
  }
  const auto parent = OpenLong(at.base, std::string_view{c_path, slash},
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (!parent) {
    errno = static_cast<int>(parent.error().code);
    return -1;
  }
  return cutils::os::unlinkat(*parent, c_path + slash + 1, AT_REMOVEDIR);
}

auto DirNode::Lookup(std::string& scratch) const noexcept -> int {
  const KernelPath at = walk_.PathOf(*this, scratch);
  struct stat node_stat;
  const int status =
      at.base == AT_FDCWD
          ? cutils::os::lstat(at.path, &node_stat)
          : ::fstatat(at.base, at.path, &node_stat, AT_SYMLINK_NOFOLLOW);
  return status == 0 ? 0 : errno;
}

}  // namespace remmy
