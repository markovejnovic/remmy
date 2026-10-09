// See LICENSE in the repository root.

#ifndef REMMY_TASK_HPP
#define REMMY_TASK_HPP

#include <cstdint>

#include "dir_node.hpp"
#include "name_batch.hpp"

namespace remmy {

/// @brief A run of a directory's files, unlinked by whichever worker takes it.
///
/// Holds a reference on `dir` (counted in `remaining_children_dirs_`, like a
/// subdirectory), which keeps the directory, and its open descriptor, alive
/// until the chunk is done.
struct UnlinkChunk {
  DirNode* dir;
  NameBatch files;
};

/// @brief What a worker runs: scan a directory, or unlink a chunk of one.
///
/// The scheduler's queues hold tasks in std::atomic, so a task stays one
/// pointer wide: the low bit says which kind it points at.
class Task {
 public:
  Task() = default;

  [[nodiscard]] static auto Scan(DirNode* dir) noexcept -> Task {
    return Task{reinterpret_cast<std::uintptr_t>(dir)};
  }

  [[nodiscard]] static auto Unlink(UnlinkChunk* chunk) noexcept -> Task {
    return Task{reinterpret_cast<std::uintptr_t>(chunk) | kUnlinkTag};
  }

  // Turning the tagged bits back into a pointer is the point of a tagged
  // pointer, so the casts below are deliberate.
  // NOLINTBEGIN(performance-no-int-to-ptr)

  /// @brief The directory to scan, or null for an unlink chunk.
  [[nodiscard]] auto AsScan() const noexcept -> DirNode* {
    return (bits_ & kUnlinkTag) != 0 ? nullptr
                                     : reinterpret_cast<DirNode*>(bits_);
  }

  /// @brief The chunk to unlink, or null for a scan.
  [[nodiscard]] auto AsUnlink() const noexcept -> UnlinkChunk* {
    return (bits_ & kUnlinkTag) != 0
               ? reinterpret_cast<UnlinkChunk*>(bits_ & ~kUnlinkTag)
               : nullptr;
  }

  // NOLINTEND(performance-no-int-to-ptr)

 private:
  static constexpr std::uintptr_t kUnlinkTag = 1;
  static_assert(alignof(DirNode) > kUnlinkTag &&
                alignof(UnlinkChunk) > kUnlinkTag);

  explicit Task(std::uintptr_t bits) noexcept : bits_(bits) {}

  std::uintptr_t bits_ = 0;
};

}  // namespace remmy

#endif  // REMMY_TASK_HPP
