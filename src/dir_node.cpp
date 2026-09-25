// See LICENSE in the repository root.

#include "dir_node.hpp"

#include <fcntl.h>

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

namespace remmy {

namespace {

/// @brief openat relative to `dir`, or to the cwd when `dir` is not open.
auto OpenAt(const cutils::os::Fd& dir, const char* name, int flags) noexcept
    -> std::expected<cutils::os::Fd, cutils::os::OpenError> {
  return dir.IsOpen() ? cutils::os::openat(dir, name, flags)
                      : cutils::os::open(name, flags);
}

/// @brief Open `path`, even when it is PATH_MAX bytes or longer.
///
/// Returns why it could not be opened on failure.
auto OpenLong(std::string_view path, int flags) noexcept
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
    auto next = OpenAt(dir, piece.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!next) {
      return std::unexpected(next.error());
    }
    dir = *std::move(next);
    // The rest is relative to `dir`, even after a doubled slash.
    path.remove_prefix(cut);
    while (!path.empty() && path.front() == '/') {
      path.remove_prefix(1);
    }
  }
  std::copy_n(path.begin(), path.size(), piece.begin());
  piece[path.size()] = '\0';
  return OpenAt(dir, piece.data(), flags);
}

}  // namespace

auto DirNode::Parents() const noexcept -> ParentChain {
  return ParentChain{this};
}

auto DirNode::ParentsMut() noexcept -> MutableParentChain {
  return MutableParentChain{this};
}

auto DirNode::PathInto(std::string& out) const -> const char* {
  // I want to avoid resizing here too much, so first we count the total
  // number of bytes we'd need in the directory tree.
  const std::size_t total = std::ranges::fold_left(
      Parents(), std::size_t{0}, [](std::size_t acc, const DirNode* t) {
        return acc + t->name_.size() +
               static_cast<std::size_t>(t->parent_ != nullptr);
      });

  out.resize_and_overwrite(total, [this](char* data, std::size_t size) {
    // This is so janky, but it is the price you pay to avoid allocations.
    //
    // We make _another_ scan through the directory nodes (hopefully they're
    // all in-cache), and we write data in reverse order.
    char* cursor = data + size;
    for (const DirNode* t : Parents()) {
      cursor -= t->name_.size();
      std::copy_n(t->name_.data(), t->name_.size(), cursor);
      if (t->parent_ != nullptr) {
        *--cursor = '/';
      }
    }
    return size;
  });
  return out.c_str();
}

auto DirNode::Open(std::string& path_buf) noexcept
    -> std::expected<void, cutils::os::OpenError> {
  if (fd_.IsOpen()) {
    return {};
  }

  auto opened = OpenLong(PathInto(path_buf),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!opened) {
    return std::unexpected(opened.error());
  }
  fd_ = *std::move(opened);
  return {};
}

auto DirNode::RemoveEmpty(std::string& scratch) const noexcept -> int {
  const char* const c_path = PathInto(scratch);
  const std::string_view path = scratch;
  if (path.size() < PATH_MAX) {
    return cutils::os::rmdir(c_path);
  }

  // Too long for rmdir: remove it relative to its parent instead.
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    errno = ENAMETOOLONG;
    return -1;
  }
  const auto parent = OpenLong(std::string_view{c_path, slash},
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (!parent) {
    errno = static_cast<int>(parent.error().code);
    return -1;
  }
  return cutils::os::unlinkat(*parent, c_path + slash + 1, AT_REMOVEDIR);
}

}  // namespace remmy
