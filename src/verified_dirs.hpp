// See LICENSE in the repository root.

#ifndef REMMY_VERIFIED_DIRS_HPP
#define REMMY_VERIFIED_DIRS_HPP

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cutils/os/fd.hpp"
#include "cutils/os/os.hpp"
#include "dir_node.hpp"
#include "entry_stat.hpp"

namespace remmy {

/// @brief The prompter's way into directories: through DirNode::OpenVerified,
///        holding the last one open.
///
/// The held descriptor is keyed by the identity it was verified as, not by
/// the DirNode, whose address a later node may reuse. From it, a directory
/// one step away is reached as fts moves, down by name or up by "..", and
/// checked the same way; anything further is walked to from its operand.
/// Removing through a descriptor on the right directory is what fts does; the
/// directory having moved since does not redirect it.
class VerifiedDirs {
 public:
  /// @brief unlinkat(2) the entry `name` of `dir`; -1 and errno on failure.
  ///
  /// Short of descriptors while the walk holds some, it waits for one: only
  /// the prompter calls this, and it may wait, since workers free theirs
  /// without ever waiting on it.
  [[nodiscard]] auto UnlinkAt(const DirNode& dir, const std::string& name,
                              int flags) noexcept -> int {
    const cutils::os::Fd* fd = Open(dir);
    if (fd == nullptr) {
      return -1;
    }
    return cutils::os::unlinkat(*fd, name.c_str(), flags);
  }

  /// @brief Stop holding `dir` open, if this does, before it is removed.
  void Forget(const DirNode& dir) noexcept {
    if (HeldFd(dir) != nullptr) {
      held_.reset();
    }
  }

 private:
  struct Held {
    EntryStat self;

    /// @brief The held directory's parent, when it has one.
    std::optional<EntryStat> parent;
    cutils::os::Fd fd;
  };

  [[nodiscard]] static auto IsSame(const EntryStat& a,
                                   const EntryStat& b) noexcept -> bool {
    return a.dev == b.dev && a.ino == b.ino;
  }

  /// @brief The held descriptor, if it is on `dir`; else null.
  [[nodiscard]] auto HeldFd(const DirNode& dir) const noexcept
      -> const cutils::os::Fd* {
    if (!held_ || dir.stat_ == nullptr || !IsSame(held_->self, *dir.stat_)) {
      return nullptr;
    }
    return &held_->fd;
  }

  /// @brief Hold `fd`, verified to be `dir`.
  auto Hold(const DirNode& dir, cutils::os::Fd fd) noexcept
      -> const cutils::os::Fd* {
    std::optional<EntryStat> parent;
    if (dir.parent_ != nullptr && dir.parent_->stat_ != nullptr) {
      parent = *dir.parent_->stat_;
    }
    return &held_
                .emplace(Held{
                    .self = *dir.stat_, .parent = parent, .fd = std::move(fd)})
                .fd;
  }

  /// @brief `dir` reached in one checked step from the held directory, if it
  ///        is the held one's child or parent.
  [[nodiscard]] auto Step(const DirNode& dir) noexcept
      -> std::optional<cutils::os::Fd> {
    static constexpr int kDown =
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    static constexpr int kUp = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    if (!held_ || dir.stat_ == nullptr) {
      return std::nullopt;
    }

    std::expected<cutils::os::Fd, cutils::os::OpenError> next =
        std::unexpected(cutils::os::OpenError{});
    if (dir.parent_ != nullptr && dir.parent_->stat_ != nullptr &&
        IsSame(held_->self, *dir.parent_->stat_)) {
      next = cutils::os::openat(held_->fd, dir.name_.c_str(), kDown);
    } else if (held_->parent && IsSame(*held_->parent, *dir.stat_)) {
      next = cutils::os::openat(held_->fd, "..", kUp);
    } else {
      return std::nullopt;
    }

    struct stat st;
    if (!next || ::fstat(next->get(), &st) != 0 || !dir.stat_->IsSameFile(st)) {
      return std::nullopt;
    }
    return *std::move(next);
  }

  /// @brief A descriptor on `dir`, or null with errno set.
  [[nodiscard]] auto Open(const DirNode& dir) noexcept
      -> const cutils::os::Fd* {
    static constexpr auto kFirstWait = std::chrono::microseconds(100);
    static constexpr std::chrono::microseconds kLongestWait =
        std::chrono::milliseconds(10);

    if (const cutils::os::Fd* held = HeldFd(dir)) {
      return held;
    }
    if (auto stepped = Step(dir)) {
      return Hold(dir, *std::move(stepped));
    }
    held_.reset();
    for (auto wait = kFirstWait;; wait = std::min(wait * 2, kLongestWait)) {
      auto opened = dir.OpenVerified();
      if (opened) {
        return Hold(dir, *std::move(opened));
      }
      if (!opened.error().retryable) {
        errno = static_cast<int>(opened.error().code);
        return nullptr;
      }
      // A backoff, not a meeting point: some worker frees a descriptor at
      // the end of its task, whenever that is.
      std::this_thread::sleep_for(wait);
    }
  }

  std::optional<Held> held_;
};

}  // namespace remmy

#endif
