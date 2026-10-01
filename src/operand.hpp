// See LICENSE in the repository root.

#ifndef REMMY_OPERAND_HPP
#define REMMY_OPERAND_HPP

#include <sys/types.h>

#include <string_view>
#include <variant>

namespace remmy {

class Operand {
 public:
  explicit constexpr Operand(const char* path) noexcept : path_(path) {}

  [[nodiscard]] constexpr auto CStr() const noexcept -> const char* {
    return path_;
  }

  [[nodiscard]] constexpr auto Path() const noexcept -> std::string_view {
    return path_;
  }

  /// @brief Whether the path goes through a symlink before its last
  ///        component: a directory on the way to it is one.
  [[nodiscard]] auto ThroughSymlink() const -> bool;

  /// @brief Whether the path follows a symlink with its trailing slash: "l/",
  ///        where `l` is not a directory itself.
  [[nodiscard]] auto FollowsTrailingLink() const -> bool;

 private:
  const char* path_;
};

struct MissingOperand {
  Operand operand;
  int error;
};

struct FileOperand {
  Operand operand;
};

struct DirectoryOperand {
  Operand operand;
  dev_t device;
};

using ResolvedOperand =
    std::variant<MissingOperand, FileOperand, DirectoryOperand>;

[[nodiscard]] auto Resolve(Operand operand) noexcept -> ResolvedOperand;

}  // namespace remmy

#endif
