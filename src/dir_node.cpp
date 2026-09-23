// See LICENSE in the repository root.

#include "dir_node.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <string>
#include <system_error>

#include "cutils/os/limits/fd.hpp"
#include "cutils/os/os.hpp"

namespace remmy {

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
    -> std::expected<void, std::errc> {
  if (fd_.IsOpen()) {
    return {};
  }

  fd_ = cutils::os::open(PathInto(path_buf),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd_.IsOpen()) {
    return {};
  }

  if (errno == EMFILE || errno == ENFILE) {
    cutils::os::limits::fd::Pool::NoteExhaustion();
  }

  return std::unexpected(static_cast<std::errc>(errno));
}

}  // namespace remmy
