// See LICENSE in the repository root.

#ifndef REMMY_ENTRY_STAT_HPP
#define REMMY_ENTRY_STAT_HPP

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>

namespace remmy {

/// @brief What a walk keeps from an entry's lstat(2): which file it is, and
///        what the override prompt prints about it.
struct EntryStat {
  dev_t dev;
  ino_t ino;
  mode_t mode;
  uid_t uid;
  gid_t gid;

  /// @brief chflags(2) flags; 0 on Linux.
  std::uint32_t flags;

  [[nodiscard]] static auto From(const struct stat& st) noexcept -> EntryStat {
#if defined(__APPLE__)
    const std::uint32_t flags = st.st_flags;
#else
    const std::uint32_t flags = 0;
#endif
    return {.dev = st.st_dev,
            .ino = st.st_ino,
            .mode = st.st_mode,
            .uid = st.st_uid,
            .gid = st.st_gid,
            .flags = flags};
  }

  /// @brief Whether `st` describes this same file.
  [[nodiscard]] auto IsSameFile(const struct stat& st) const noexcept -> bool {
    return st.st_dev == dev && st.st_ino == ino;
  }
};

}  // namespace remmy

#endif
