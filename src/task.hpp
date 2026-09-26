// See LICENSE in the repository root.

#ifndef REMMY_TASK_HPP
#define REMMY_TASK_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace remmy {

struct DirNode;

/// @brief What a scheduled task is.
enum class TaskKind : std::uint8_t {
  /// A directory to open and scan. The task is a [`DirNode`].
  kScanDir,

  /// A run of names to unlink inside an already-open directory. The task is
  /// an [`UnlinkBatch`].
  kUnlinkBatch,
};

/// @brief Common header of everything the scheduler carries.
///
/// The work-stealing queue stores each task in a `std::atomic<T>`, so a task
/// must stay pointer-sized. Tasks are therefore `Task*`, and the worker reads
/// [`kind`] to cast down to the concrete type.
struct Task {
  TaskKind kind;
};

/// @brief Names from one large directory, handed to another worker to unlink.
///
/// A directory is otherwise scanned and emptied by a single worker, so a flat
/// directory of many files runs on one thread. Once a scan has unlinked
/// [`kInlineFiles`] files itself, it packs the remaining file names into
/// batches that any worker can steal.
///
/// Each batch holds one of its directory's [`DirNode::fd_users_`], so the
/// directory's descriptor stays open, and the directory itself is not removed,
/// until every batch has finished.
struct UnlinkBatch : Task {
  /// @brief Files a scan unlinks itself before it starts batching.
  ///
  /// Directories below this never batch, so ordinary trees see no change.
  static constexpr std::size_t kInlineFiles = 1024;

  /// @brief Most names per batch. At ~20 µs per unlink, a full batch is
  ///        ~10 ms of work: coarse enough to amortise the hand-off, fine
  ///        enough to spread across workers.
  static constexpr std::size_t kMaxNames = 512;

  /// @brief Bytes of packed, NUL-terminated names.
  static constexpr std::size_t kNameBytes = 16UZ * 1024UZ;

  /// @brief The directory the names live in. Its descriptor is open for as
  ///        long as this batch exists.
  DirNode* dir;

  /// @brief How many names are packed.
  std::size_t count = 0;

  /// @brief Bytes of [`names`] in use.
  std::size_t used = 0;

  /// @brief Names, each followed by a NUL, back to back.
  char names[kNameBytes];

  explicit UnlinkBatch(DirNode* owner) noexcept
      : Task{TaskKind::kUnlinkBatch}, dir(owner) {}

  /// @brief Append a name, or return false if the batch is full.
  [[nodiscard]] auto TryAppend(std::string_view name) noexcept -> bool {
    if (count == kMaxNames || used + name.size() + 1 > kNameBytes) {
      return false;
    }
    std::memcpy(names + used, name.data(), name.size());
    names[used + name.size()] = '\0';
    used += name.size() + 1;
    ++count;
    return true;
  }

  /// @brief Call `fn(const char*)` for every packed name, in order.
  void ForEach(auto&& fn) const noexcept {
    for (std::size_t offset = 0; offset < used;) {
      const char* name = names + offset;
      fn(name);
      offset += std::strlen(name) + 1;
    }
  }
};

}  // namespace remmy

#endif
