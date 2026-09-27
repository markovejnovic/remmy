// See LICENSE in the repository root.

#ifndef REMMY_WALK_HPP
#define REMMY_WALK_HPP

#include <sys/types.h>

#include <cutils/os/fd.hpp>
#include <string>
#include <variant>

#include "operand.hpp"

namespace remmy {

struct DirNode;

struct KernelPath {
  int base;
  const char* path;
};

class Walk {
 public:
  [[nodiscard]] static auto Start(const DirectoryOperand& operand,
                                  const cutils::os::Fd& root) noexcept -> Walk;

  /// @brief -x: the device of the operand, which the walk does not leave.
  [[nodiscard]] auto Device() const noexcept -> dev_t { return device_; }

  [[nodiscard]] auto PathOf(const DirNode& node,
                            std::string& scratch) const noexcept -> KernelPath;

 private:
  struct ByTypedPath {};

  /// @brief The directories below the root are reached through the root's
  ///        descriptor rather than through the operand as typed.
  ///
  /// Set for an operand that goes through a symlink ("l/", "e/l/e"), which
  /// the walk may remove: a link to "." or ".." lies inside the tree it
  /// leads to. rm chdirs into the root, so its walk does not depend on the
  /// way in once it is in. The root itself is still removed by its typed
  /// name, as rm removes it, so a link removed on the way makes that fail.
  struct RelativeTo {
    cutils::os::Fd root;
  };

  Walk(dev_t device, std::variant<ByTypedPath, RelativeTo> access) noexcept
      : device_(device), access_(std::move(access)) {}

  dev_t device_;
  std::variant<ByTypedPath, RelativeTo> access_;
};

}  // namespace remmy

#endif
