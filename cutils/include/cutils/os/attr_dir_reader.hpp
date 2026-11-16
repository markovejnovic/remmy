// See LICENSE in the repository root.

/// @brief Reading a directory together with what deciding rm's override
///        prompt takes: each entry's kind, file flags, and whether this
///        process may write it.
///
/// On macOS getattrlistbulk(2) returns all of that for a whole buffer of
/// entries in one call; ATTR_CMN_USERACCESS is the kernel's own access check,
/// ACLs included. Elsewhere, or on a volume that refuses getattrlistbulk,
/// each entry costs a faccessat(2), plus an fstatat(2) where its kind or
/// flags are not in the listing.
#ifndef CUTILS_OS_ATTR_DIR_READER_HPP
#define CUTILS_OS_ATTR_DIR_READER_HPP

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>
#include <system_error>
#include <vector>

#include "cutils/os/fd.hpp"
#include "cutils/os/os.hpp"

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/vnode.h>
#endif

namespace cutils::os {

/// @brief What an entry is, as far as removing it goes.
enum class EntryKind : std::uint8_t { kDirectory, kSymlink, kOther };

/// @brief One directory entry and its attributes.
class AttrEntry {
 public:
  constexpr AttrEntry(const char* name, std::size_t length, EntryKind kind,
                      std::uint32_t flags, bool writable, int error) noexcept
      : name_(name),
        length_(length),
        kind_(kind),
        flags_(flags),
        writable_(writable),
        error_(error) {}

  [[nodiscard]] constexpr auto name() const noexcept -> std::string_view {
    return {name_, length_};
  }

  /// @brief The name, NUL-terminated, as the *at syscalls take it.
  [[nodiscard]] constexpr auto c_str() const noexcept -> const char* {
    return name_;
  }

  [[nodiscard]] constexpr auto kind() const noexcept -> EntryKind {
    return kind_;
  }

  /// @brief chflags(2) flags; always 0 on Linux.
  [[nodiscard]] constexpr auto flags() const noexcept -> std::uint32_t {
    return flags_;
  }

  /// @brief Whether access(2) would allow this process W_OK; meaningless
  ///        for a symlink.
  [[nodiscard]] constexpr auto writable() const noexcept -> bool {
    return writable_;
  }

  /// @brief errno for an entry whose attributes could not be read, else 0.
  ///        Only name() means anything when it is set.
  [[nodiscard]] constexpr auto error() const noexcept -> int { return error_; }

 private:
  const char* name_;
  std::size_t length_;
  EntryKind kind_;
  std::uint32_t flags_;
  bool writable_;
  int error_;
};

/// @brief Reads directories with AttrEntry attributes, reusing one buffer.
class AttrDirReader {
 public:
  /// @brief How entries' attributes are fetched.
  enum class Method : std::uint8_t {
    /// getattrlistbulk(2) on macOS; on Linux the same as kPerEntry.
    kBulk,
    /// A listing, then faccessat(2) and, where needed, fstatat(2) per entry.
    kPerEntry,
  };

  explicit AttrDirReader(Method method = Method::kBulk)
      : method_(method), buffer_(kBufferWords) {}

  /// @brief Call `visit` with each entry of `dir` but . and .., in the
  ///        order the directory lists them.
  ///
  /// @return Why reading stopped before the end, if it did; `visit` has
  ///         seen every entry read until then.
  template <class Visit>
  [[nodiscard]] auto ForEach(const Fd& dir, Visit&& visit)
      -> std::expected<void, std::errc> {
#if defined(__APPLE__)
    if (method_ == Method::kBulk) {
      const auto bulk = ForEachBulk(dir, visit);
      if (bulk || bulk.error() != kUnsupported) {
        return bulk;
      }
    }
#endif
    return ForEachPerEntry(dir, visit);
  }

 private:
  static constexpr std::size_t kBufferWords = 16UZ * 1024UZ;

  /// @brief The bulk read failed before any entry: use the per-entry one.
  static constexpr std::errc kUnsupported = std::errc::not_supported;

  template <class Visit>
  auto ForEachPerEntry(const Fd& dir, Visit& visit)
      -> std::expected<void, std::errc> {
    for (const auto& read : dirs_.Read(dir)) {
      if (!read) {
        return std::unexpected(read.error());
      }
      const DirEntry entry = *read;
      if (entry.is_dot_or_dot_dot()) {
        continue;
      }
      visit(Describe(dir, entry));
    }
    return {};
  }

  [[nodiscard]] static auto Describe(const Fd& dir,
                                     const DirEntry& entry) noexcept
      -> AttrEntry {
    EntryKind kind = KindOf(entry.type());
    std::uint32_t flags = 0;
#if defined(__APPLE__)
    // getdirentries64 does not list flags, so every entry needs a stat.
    const bool needs_stat = true;
#else
    const bool needs_stat = entry.is_type_unknown();
#endif
    if (needs_stat) {
      struct stat st;
      if (fstatat(dir, entry.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return {entry.c_str(), entry.name().size(), kind, 0, false, errno};
      }
      kind = S_ISDIR(st.st_mode)   ? EntryKind::kDirectory
             : S_ISLNK(st.st_mode) ? EntryKind::kSymlink
                                   : EntryKind::kOther;
#if defined(__APPLE__)
      flags = st.st_flags;
#endif
    }

    bool writable = false;
    if (kind != EntryKind::kSymlink) {
      writable = ::faccessat(dir.get(), entry.c_str(), W_OK, 0) == 0;
      if (!writable && errno == ENOENT) {
        return {entry.c_str(), entry.name().size(), kind, flags, false, ENOENT};
      }
    }
    return {entry.c_str(), entry.name().size(), kind, flags, writable, 0};
  }

  [[nodiscard]] static constexpr auto KindOf(std::uint8_t type) noexcept
      -> EntryKind {
    switch (type) {
      case DT_DIR:
        return EntryKind::kDirectory;
      case DT_LNK:
        return EntryKind::kSymlink;
      default:
        return EntryKind::kOther;
    }
  }

#if defined(__APPLE__)
  /// @brief Read one value of type T at `at`, advancing past it.
  template <class T>
  [[nodiscard]] static auto Take(const std::byte*& at) noexcept -> T {
    T value;
    std::memcpy(&value, at, sizeof(T));
    at += sizeof(T);
    return value;
  }

  template <class Visit>
  auto ForEachBulk(const Fd& dir, Visit& visit)
      -> std::expected<void, std::errc> {
    attrlist request{};
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME |
                         ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_FLAGS |
                         ATTR_CMN_USERACCESS;
    bool any = false;

    while (true) {
      const int count =
          ::getattrlistbulk(dir.get(), &request, buffer_.data(),
                            buffer_.size() * sizeof(buffer_[0]), 0);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        const int error = errno;
        if (!any && (error == ENOTSUP || error == EINVAL)) {
          return std::unexpected(kUnsupported);
        }
        return std::unexpected(static_cast<std::errc>(error));
      }
      if (count == 0) {
        return {};
      }
      any = true;

      const auto* entry = reinterpret_cast<const std::byte*>(buffer_.data());
      for (int i = 0; i < count; ++i) {
        const std::byte* at = entry;
        const auto length = Take<std::uint32_t>(at);
        const auto returned = Take<attribute_set_t>(at);
        const std::uint32_t got = returned.commonattr;

        int error = 0;
        if ((got & ATTR_CMN_ERROR) != 0) {
          error = static_cast<int>(Take<std::uint32_t>(at));
        }
        const char* name = "";
        std::size_t name_length = 0;
        if ((got & ATTR_CMN_NAME) != 0) {
          const std::byte* reference = at;
          const auto name_info = Take<attrreference_t>(at);
          name = reinterpret_cast<const char*>(reference +
                                               name_info.attr_dataoffset);
          // attr_length counts the terminating NUL.
          name_length =
              name_info.attr_length > 0 ? name_info.attr_length - 1 : 0;
        }
        EntryKind kind = EntryKind::kOther;
        if ((got & ATTR_CMN_OBJTYPE) != 0) {
          const auto type = Take<fsobj_type_t>(at);
          kind = type == VDIR   ? EntryKind::kDirectory
                 : type == VLNK ? EntryKind::kSymlink
                                : EntryKind::kOther;
        }
        std::uint32_t flags = 0;
        if ((got & ATTR_CMN_FLAGS) != 0) {
          flags = Take<std::uint32_t>(at);
        }
        bool writable = false;
        if ((got & ATTR_CMN_USERACCESS) != 0) {
          writable = (Take<std::uint32_t>(at) & W_OK) != 0;
        }
        if (error == 0 && kind != EntryKind::kSymlink &&
            (got & ATTR_CMN_USERACCESS) == 0) {
          // A volume that cannot say: ask the kernel the slow way.
          writable = ::faccessat(dir.get(), name, W_OK, 0) == 0;
          if (!writable && errno == ENOENT) {
            error = ENOENT;
          }
        }

        visit(AttrEntry{name, name_length, kind, flags, writable, error});
        entry += length;
      }
    }
  }
#endif

  Method method_;

  /// @brief getattrlistbulk's buffer, in words for its 4-byte alignment.
  std::vector<std::uint32_t> buffer_;
  DirReader dirs_;
};

}  // namespace cutils::os

#endif  // CUTILS_OS_ATTR_DIR_READER_HPP
