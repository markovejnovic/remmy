// See LICENSE in the repository root.

/// @brief OS utilities that are better (in some ways) than stdlibc++.
#ifndef CUTILS_OS_OS_HPP
#define CUTILS_OS_OS_HPP

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cutils/os/fd.hpp>
#include <expected>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
// These are the libsystem_kernel symbols themselves, so the reserved spelling
// is the point rather than an accident.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" int __unlinkat(int fd, const char* name, int opts);
extern "C" auto __getdirentries64(int fd, void* buffer, std::size_t size,
                                  off_t* offset) -> ssize_t;
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#elif !defined(__linux__)
#error "cutils/os/os.hpp: requires Darwin or Linux"
#endif

namespace cutils::os {

/// @brief The record layout the raw directory-read syscall fills in.
#if defined(__APPLE__)
using RawDirent = ::dirent;
#else
// getdents64 always writes the 64-bit layout, whatever ::dirent is here.
using RawDirent = ::dirent64;
#endif

/// @brief Returns an owning Fd; an empty one means failure, with errno set.
[[nodiscard]] inline auto open(const char* path, int flags) noexcept -> Fd {
  return Fd{::open(path, flags)};
}

/// @brief Returns an owning Fd; an empty one means failure, with errno set.
[[nodiscard]] inline auto openat(const Fd& dir, const char* name,
                                 int flags) noexcept -> Fd {
  return Fd{::openat(dir.get(), name, flags)};
}

inline auto lstat(const char* path, struct ::stat* out) noexcept -> int {
  return ::lstat(path, out);
}

inline auto fstatat(const Fd& dir, const char* name, struct ::stat* out,
                    int flags) noexcept -> int {
  return ::fstatat(dir.get(), name, out, flags);
}

inline auto unlink(const char* path) noexcept -> int { return ::unlink(path); }

inline auto rmdir(const char* path) noexcept -> int { return ::rmdir(path); }

inline auto unlinkat(const Fd& dir, const char* name, int opts) noexcept
    -> int {
#ifdef __APPLE__
  // The unlinkat libsyscall wrapper increments an atomic we do not care about.
  //
  // To save that extra bit of compute and the pipeline stall, we skip that
  // directly and just call the syscall itself.
  return __unlinkat(dir.get(), name, opts);
#else
  return ::unlinkat(dir.get(), name, opts);
#endif
}

inline auto getdirentries64(const Fd& dir, void* buffer, std::size_t size,
                            off_t* offset) noexcept -> ssize_t {
#ifdef __APPLE__
  // getdirentries() is poisoned once 64-bit inodes are in effect, and readdir()
  // costs a DIR struct plus a read buffer per directory. This is the stub
  // readdir() itself calls, so we read straight into a buffer we already own.
  return __getdirentries64(dir.get(), buffer, size, offset);
#else
  // getdents64 tracks the position in the open file description itself.
  (void)offset;
  return ::getdents64(dir.get(), buffer, size);
#endif
}

class DirEntry {
 public:
  explicit DirEntry(const RawDirent* raw) noexcept : raw_(raw) {}

  [[nodiscard]] auto name() const noexcept -> std::string_view {
#if defined(__APPLE__)
    return {static_cast<const char*>(raw_->d_name), raw_->d_namlen};
#else
    // Linux records carry no name length; the name is NUL-padded instead.
    const auto* text = static_cast<const char*>(raw_->d_name);
    return {text, std::strlen(text)};
#endif
  }

  [[nodiscard]] auto type() const noexcept -> std::uint8_t {
    return raw_->d_type;
  }

  [[nodiscard]] auto is_directory() const noexcept -> bool {
    return raw_->d_type == DT_DIR;
  }

  // Some filesystems leave d_type unset; the caller must fstatat to decide.
  [[nodiscard]] auto is_type_unknown() const noexcept -> bool {
    return raw_->d_type == DT_UNKNOWN;
  }

  [[nodiscard]] auto is_dot_or_dot_dot() const noexcept -> bool {
    const std::string_view text = name();
    return text == "." || text == "..";
  }

  // NUL-terminated, as the *at syscalls require.
  [[nodiscard]] auto c_str() const noexcept -> const char* {
    return static_cast<const char*>(raw_->d_name);
  }

  [[nodiscard]] auto raw() const noexcept -> const RawDirent& { return *raw_; }

 private:
  const RawDirent* raw_;
};

inline constexpr std::size_t kDefaultDirBufferBytes = 64UZ * 1024UZ;

// One directory's entries, read in blocks through a borrowed buffer.
class DirEntries : public std::ranges::view_interface<DirEntries> {
 public:
  // Storage aligned for RawDirent so the reinterpret_cast in CurrentEntry is
  // valid; alignment tracks RawDirent by construction rather than by
  // coincidence.
  struct alignas(RawDirent) Word {
    std::byte storage[alignof(RawDirent)];
  };
  using value_type = std::expected<DirEntry, std::errc>;

  DirEntries(const Fd& dir, std::span<Word> buffer) noexcept
      : dir_(&dir), buffer_(buffer) {
    Fill();
  }

  class iterator {
   public:
    using value_type = DirEntries::value_type;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;

    iterator() noexcept = default;
    explicit iterator(DirEntries* owner) noexcept : owner_(owner) {}

    [[nodiscard]] auto operator*() const noexcept -> value_type {
      return owner_->Current();
    }

    auto operator++() noexcept -> iterator& {
      owner_->Advance();
      return *this;
    }

    void operator++(int) noexcept { ++*this; }

    [[nodiscard]] auto operator==(std::default_sentinel_t) const noexcept
        -> bool {
      return owner_ == nullptr || owner_->AtEnd();
    }

   private:
    DirEntries* owner_ = nullptr;
  };

  [[nodiscard]] auto begin() noexcept -> iterator { return iterator{this}; }

  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

 private:
  [[nodiscard]] auto AtEnd() const noexcept -> bool {
    return !failed_ && offset_ >= filled_;
  }

  [[nodiscard]] auto CurrentEntry() const noexcept -> DirEntry {
    // Entries are packed and d_reclen-strided; sizeof(dirent) is the maximum,
    // not the stride.
    const auto* bytes = reinterpret_cast<const char*>(buffer_.data());
    return DirEntry{reinterpret_cast<const RawDirent*>(bytes + offset_)};
  }

  [[nodiscard]] auto Current() const noexcept -> value_type {
    if (failed_) {
      return std::unexpected(error_);
    }
    return CurrentEntry();
  }

  void Advance() noexcept {
    if (failed_) {
      failed_ = false;  // Reported once, then the range ends.
      filled_ = 0;
      offset_ = 0;
      return;
    }
    offset_ += CurrentEntry().raw().d_reclen;
    if (offset_ >= filled_) {
      Fill();
    }
  }

  void Fill() noexcept {
    offset_ = 0;
    filled_ = 0;
    for (;;) {
      const ssize_t bytes = getdirentries64(*dir_, buffer_.data(),
                                            buffer_.size_bytes(), &position_);
      if (bytes >= 0) {
        filled_ = static_cast<std::size_t>(bytes);
        return;
      }
      if (errno == EINTR) {
        continue;  // A signal is not the end of the directory.
      }
      error_ = static_cast<std::errc>(errno);
      failed_ = true;
      return;
    }
  }

  const Fd* dir_;
  std::span<Word> buffer_;
  off_t position_ = 0;
  std::size_t filled_ = 0;
  std::size_t offset_ = 0;
  std::errc error_{};
  bool failed_ = false;
};

// Owns a read buffer sized once and reused for every directory it reads.
template <typename Allocator = std::allocator<DirEntries::Word>>
class BasicDirReader {
 public:
  // Separate from the sized constructor so that `explicit` does not make a
  // DirReader member unusable in an aggregate: aggregate initialisation
  // copy-initialises a member from {}, which cannot pick an explicit
  // constructor.
  BasicDirReader() : BasicDirReader(kDefaultDirBufferBytes) {}

  explicit BasicDirReader(std::size_t buffer_bytes,
                          const Allocator& allocator = Allocator{})
      : buffer_((buffer_bytes + sizeof(DirEntries::Word) - 1) /
                    sizeof(DirEntries::Word),
                allocator) {}

  [[nodiscard]] auto Read(const Fd& dir) noexcept -> DirEntries {
    return DirEntries{dir, buffer_};
  }

 private:
  std::vector<DirEntries::Word, Allocator> buffer_;
};

using DirReader = BasicDirReader<>;

}  // namespace cutils::os

#endif
