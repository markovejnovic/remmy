// See LICENSE in the repository root.

#include "dir_node.hpp"

#include <fcntl.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "cutils/os/os.hpp"

namespace remmy {

auto DirNode::Parents() const noexcept -> ParentChain {
  return ParentChain{this};
}

auto DirNode::ParentsMut() noexcept -> MutableParentChain {
  return MutableParentChain{this};
}

auto DirNode::PathInto(std::string& out, std::string_view root) const
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

auto DirNode::Open(std::string& scratch, const cutils::os::Fd* root) noexcept
    -> std::expected<void, cutils::os::OpenError> {
  if (fd_.IsOpen()) {
    return {};
  }

  constexpr int kFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  auto opened = root != nullptr
                    ? cutils::os::openat(*root, PathInto(scratch, "."), kFlags)
                    : cutils::os::open(PathInto(scratch), kFlags);
  if (!opened && opened.error().code == std::errc::filename_too_long &&
      parent_ != nullptr) {
    auto parent = OpenParent(scratch, root);
    if (!parent) {
      return std::unexpected(parent.error());
    }
    opened = cutils::os::openat(*parent, name_.c_str(), kFlags);
  }
  if (!opened) {
    return std::unexpected(opened.error());
  }
  fd_ = *std::move(opened);
  return {};
}

auto DirNode::OpenParent(std::string& scratch,
                         const cutils::os::Fd* root) const noexcept
    -> std::expected<cutils::os::Fd, cutils::os::OpenError> {
  constexpr int kFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
  // The longest path open(2) takes, its terminating NUL aside.
  constexpr std::size_t kMaxPiece = PATH_MAX - 1;

  if (parent_ == nullptr) {
    return std::unexpected(cutils::os::OpenError{
        .code = std::errc::not_a_directory, .retryable = false});
  }
  parent_->PathInto(scratch, root != nullptr ? "." : "");

  // Each piece ends at a slash, which is overwritten with the NUL that ends
  // it, and is opened relative to the directory the one before it opened.
  cutils::os::Fd base;
  std::size_t start = 0;
  while (true) {
    const bool last = scratch.size() - start <= kMaxPiece;
    std::size_t end = scratch.size();
    if (!last) {
      end = scratch.rfind('/', start + kMaxPiece);
      if (end == std::string::npos || end <= start) {
        // A single name longer than PATH_MAX; open(2) would say the same.
        return std::unexpected(cutils::os::OpenError{
            .code = std::errc::filename_too_long, .retryable = false});
      }
      scratch[end] = '\0';
    }

    const char* piece = start < scratch.size() ? scratch.c_str() + start : ".";
    auto opened = base.IsOpen()    ? cutils::os::openat(base, piece, kFlags)
                  : root != nullptr ? cutils::os::openat(*root, piece, kFlags)
                                    : cutils::os::open(piece, kFlags);
    if (!opened) {
      return std::unexpected(opened.error());
    }
    base = *std::move(opened);
    if (last) {
      return base;
    }
    scratch[end] = '/';
    // Doubled slashes would start the next piece at the root.
    start = std::min(scratch.find_first_not_of('/', end), scratch.size());
  }
}

}  // namespace remmy
