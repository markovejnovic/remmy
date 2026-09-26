// See LICENSE in the repository root.

#include "dir_node.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
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

auto DirNode::Open(std::string& scratch, std::string_view root) noexcept
    -> std::expected<void, cutils::os::OpenError> {
  if (fd_.IsOpen()) {
    return {};
  }

  auto opened = cutils::os::open(
      PathInto(scratch, root), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!opened) {
    return std::unexpected(opened.error());
  }
  fd_ = *std::move(opened);
  return {};
}

}  // namespace remmy
