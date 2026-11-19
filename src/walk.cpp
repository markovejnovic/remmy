// See LICENSE in the repository root.

#include "walk.hpp"

#include <fcntl.h>

#include <string>
#include <utility>
#include <variant>

#include "dir_node.hpp"

namespace remmy {

auto Walk::Start(const DirectoryOperand& operand,
                 const cutils::os::Fd& root) noexcept -> Walk {
  if (operand.operand.FollowsTrailingLink() ||
      operand.operand.ThroughSymlink()) {
    auto kept = cutils::os::Fd::Open(
        [&] { return ::fcntl(root.get(), F_DUPFD_CLOEXEC, 0); });
    if (kept) {
      return {operand.device, RelativeTo{.root = *std::move(kept)}};
    }
  }
  return {operand.device, ByTypedPath{}};
}

auto Walk::PathOf(const DirNode& node, std::string& scratch) const noexcept
    -> KernelPath {
  const auto* relative = std::get_if<RelativeTo>(&access_);
  if (node.parent_ == nullptr || relative == nullptr) {
    return {.base = AT_FDCWD, .path = node.PathInto(scratch)};
  }
  return {.base = relative->root.get(), .path = node.PathInto(scratch, ".")};
}

}  // namespace remmy
