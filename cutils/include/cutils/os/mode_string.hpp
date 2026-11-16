// See LICENSE in the repository root.

#ifndef CUTILS_OS_MODE_STRING_HPP
#define CUTILS_OS_MODE_STRING_HPP

#include <sys/stat.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace cutils::os {

/// @brief strmode(3)'s text for a mode: its type, nine permission letters
///        and a space, such as `-rw-r--r-- `.
///
/// A constexpr port, since glibc has no strmode.
class ModeString {
 public:
  explicit constexpr ModeString(mode_t mode) noexcept {
    std::size_t at = 0;
    chars_[at++] = TypeLetter(mode);
    for (const Triad& triad : kTriads) {
      chars_[at++] = (mode & triad.read) != 0 ? 'r' : '-';
      chars_[at++] = (mode & triad.write) != 0 ? 'w' : '-';
      const bool exec = (mode & triad.exec) != 0;
      const bool special = (mode & triad.special) != 0;
      chars_[at++] = special ? (exec ? triad.special_exec : triad.special_only)
                             : (exec ? 'x' : '-');
    }
    chars_[at] = ' ';
  }

  /// @brief All eleven characters.
  [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
    return {chars_.data(), chars_.size()};
  }

 private:
  /// @brief One of owner, group and other, and the bit that shares its x.
  struct Triad {
    mode_t read;
    mode_t write;
    mode_t exec;
    mode_t special;
    char special_exec;
    char special_only;
  };

  static constexpr std::array<Triad, 3> kTriads{{
      {S_IRUSR, S_IWUSR, S_IXUSR, S_ISUID, 's', 'S'},
      {S_IRGRP, S_IWGRP, S_IXGRP, S_ISGID, 's', 'S'},
      {S_IROTH, S_IWOTH, S_IXOTH, S_ISVTX, 't', 'T'},
  }};

  [[nodiscard]] static constexpr auto TypeLetter(mode_t mode) noexcept -> char {
    switch (mode & S_IFMT) {
      case S_IFDIR:
        return 'd';
      case S_IFCHR:
        return 'c';
      case S_IFBLK:
        return 'b';
      case S_IFREG:
        return '-';
      case S_IFLNK:
        return 'l';
      case S_IFSOCK:
        return 's';
      case S_IFIFO:
        return 'p';
#if defined(S_IFWHT)
      case S_IFWHT:
        return 'w';
#endif
      default:
        return '?';
    }
  }

  std::array<char, 11> chars_{};
};

}  // namespace cutils::os

#endif  // CUTILS_OS_MODE_STRING_HPP
