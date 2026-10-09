// See LICENSE in the repository root.

#ifndef REMMY_NAME_BATCH_HPP
#define REMMY_NAME_BATCH_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace remmy {

/// @brief Names of directory entries, kept with their inode numbers so they
///        can be removed in inode order.
///
/// APFS keys a file's inode, attribute and extent records by its object ID,
/// which is its inode number, while directory reads come back in name-hash
/// order. Unlinking in inode order makes consecutive unlinks touch neighbouring
/// B-tree leaves instead of jumping across the volume's tree.
///
/// Names are copied into one buffer, so a batch outlives the directory read
/// buffer it was filled from.
class NameBatch {
 public:
  struct Entry {
    std::uint64_t ino;
    std::uint32_t offset;
  };

  void Add(std::uint64_t ino, std::string_view name) {
    entries_.push_back(
        {.ino = ino, .offset = static_cast<std::uint32_t>(names_.size())});
    names_.append(name);
    names_.push_back('\0');
  }

  void SortByInode() noexcept { std::ranges::sort(entries_, {}, &Entry::ino); }

  /// @brief The entry's name, NUL-terminated as the *at syscalls require.
  [[nodiscard]] auto Name(const Entry& entry) const noexcept -> const char* {
    return names_.data() + entry.offset;
  }

  [[nodiscard]] auto Entries() const noexcept -> std::span<const Entry> {
    return entries_;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_.size();
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }

  void Clear() noexcept {
    entries_.clear();
    names_.clear();
  }

 private:
  std::vector<Entry> entries_;
  std::string names_;
};

}  // namespace remmy

#endif  // REMMY_NAME_BATCH_HPP
