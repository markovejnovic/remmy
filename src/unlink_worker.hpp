// See LICENSE in the repository root.

/// @brief The scheduler's worker: walks directories, unlinks files and
///        schedules subdirectories, asking first where the policy says to.
#ifndef REMMY_UNLINK_WORKER_HPP
#define REMMY_UNLINK_WORKER_HPP

#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "cutils/os/fd.hpp"
#include "cutils/os/limits/fd.hpp"
#include "cutils/os/os.hpp"
#include "dir_node.hpp"
#include "remover.hpp"
#include "request.hpp"

namespace remmy {

/// @brief Traverses directory, unlinks files, schedules subdirs as tasks.
///
/// Do note that this type is **stateful** across multiple tasks. The scheduler
/// I've written will inject new tasks via [`Process`].
template <class Policy>
class FileUnlinkWorker {
 public:
  using task_type = DirNode*;

  static constexpr std::size_t kRunnable = 0;
  static constexpr std::size_t kAwaitingDescriptor = 1;
  static constexpr std::size_t kRanks = 2;

  explicit FileUnlinkWorker(Remover<Policy> remover) noexcept
      : remover_(std::move(remover)) {}

  /// @brief The total number of failures that this worker encountered.
  [[nodiscard]] auto Failures() const noexcept -> std::size_t {
    return remover_.Failures();
  }

  /// @brief Write output held back by a prompt. Call once the workers have
  ///        stopped.
  void FlushHeld() noexcept { remover_.Out().Release(); }

  /// @brief The main entry-point the scheduler invokes for this task.
  ///
  /// This function is called by the scheduler periodically as new tasks are
  /// admitted into the scheduler.
  void Process(DirNode* task, auto& ctx) noexcept {
    ProcessTask(task, ctx);
    remover_.Out().Release();
  }

 private:
  void ProcessTask(DirNode* task, auto& ctx) noexcept {
    auto open_result = task->Open(path_buffer_);
    if (!open_result) {
      const cutils::os::OpenError err = open_result.error();
      if (err.retryable) {
        // Out of descriptors, but one of ours is or will be freed: park the
        // directory and retry it later.
        ctx.Submit(task, kAwaitingDescriptor);
      } else {
        if (!remover_.Command().Options().force ||
            (remover_.RemoveEmptyLogged(task) != 0 && errno != ENOENT)) {
          remover_.Fail(task->PathInto(path_buffer_),
                        static_cast<int>(err.code));
        }

        DirNode* parent = task->parent_;
        delete task;
        if (parent != nullptr) {
          remover_.Release(parent, ctx);
        }
      }

      return;
    }

    Scan(task, ctx);
    task->fd_.Close();

    remover_.Release(task, ctx);
  }

  /// @brief Scan through the given directory.
  void Scan(DirNode* task, auto& ctx) noexcept {
    const bool verbose = remover_.Command().Options().verbose;
    const bool force = remover_.Command().Options().force;
    // Entries are logged as "<dir>/<name>", fts's path for them.
    const std::string_view dir_path = verbose ? task->PathInto(scan_path_) : "";
    for (const auto& read : dirs_.Read(task->fd_)) {
      if (!read) {
        // A failed read is not the end of the directory; say so and stop.
        remover_.Fail(task->PathInto(path_buffer_),
                      static_cast<int>(read.error()));
        break;
      }

      const cutils::os::DirEntry entry = *read;
      if (entry.is_dot_or_dot_dot()) {
        continue;
      }

      bool is_dir = entry.is_directory();
      if (entry.is_type_unknown()) {
        // Some filesystems don't populate d_type; fall back to fstatat.
        struct stat st;
        if (cutils::os::fstatat(task->fd_, entry.c_str(), &st,
                                AT_SYMLINK_NOFOLLOW) != 0) {
          if (errno != ENOENT) {
            remover_.Fail(task->PathInto(path_buffer_), entry.name(), errno);
          }
          continue;
        }
        is_dir = S_ISDIR(st.st_mode);
      }

      if constexpr (Policy::kAsksEveryEntry) {
        Ask(task, entry.name(), is_dir, ctx);
        continue;
      }

      if (!is_dir) {
        if (remover_.Logged(cutils::os::unlinkat(task->fd_, entry.c_str(), 0),
                            "{}/{}", dir_path, entry.name()) != 0 &&
            errno != ENOENT) {
          remover_.Fail(task->PathInto(path_buffer_), entry.name(), errno);
        }
        continue;
      }

      // Near the ceiling, skip the attempt and park the child straight away.
      cutils::os::Fd child_fd;
      if (!cutils::os::limits::fd::Pool::Exhausted()) {
        auto opened =
            cutils::os::openat(task->fd_, entry.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (opened) {
          child_fd = *std::move(opened);
        } else if (!opened.error().retryable) {
          if (!force ||
              (remover_.Logged(
                   cutils::os::unlinkat(task->fd_, entry.c_str(), AT_REMOVEDIR),
                   "{}/{}", dir_path, entry.name()) != 0 &&
               errno != ENOENT)) {
            remover_.Fail(task->PathInto(path_buffer_), entry.name(),
                          static_cast<int>(opened.error().code));
          }
          continue;
        }
      }

      // Count the child on the parent before publishing it: a worker could
      // steal and finish it the instant Submit returns, and the parent's own
      // scan reference (held until Finish, below) keeps
      // `remaining_children_dirs_` from reaching zero mid-scan regardless.
      auto* child =
          new DirNode(std::move(child_fd), task, std::string{entry.name()});
      task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
      const std::size_t tier =
          child->fd_.IsOpen() ? kRunnable : kAwaitingDescriptor;

      // The scheduler carries raw pointers (its slots are trivially copyable
      // and a steal may briefly duplicate one), so ownership is manual: the
      // node is freed by whichever worker drops its last reference in Finish.
      ctx.Submit(child, tier);
    }
  }

  /// @brief Hand the entry `name` of `task` to the prompter instead of
  ///        removing or entering it.
  void Ask(DirNode* task, std::string_view name, bool is_dir,
           auto& ctx) noexcept {
    // Counted on the directory first, as for a scheduled child, so it
    // outlives the question.
    task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
    if (!is_dir) {
      remover_.Requests().Push(ConfirmRemove{
          .parent = task, .name = std::string{name}, .token = ctx.Hold()});
      return;
    }

    // Not opened: a question may wait a long time, and must not hold a
    // descriptor while it does. Process opens it by path once answered.
    remover_.Requests().Push(ConfirmExamine{
        .dir = new DirNode(cutils::os::Fd{}, task, std::string{name}),
        .token = ctx.Hold()});
  }

  Remover<Policy> remover_;

  /// @brief Thread-local buffer used to compute the abspath of DirNode.
  std::string path_buffer_;

  /// @brief Thread-local iterator used to iterate over directories. Reset on
  ///        each new task.
  cutils::os::DirReader dirs_;

  /// @brief Thread-local buffer holding the path of the directory being
  ///        scanned, for -v's lines about its entries.
  std::string scan_path_;
};

}  // namespace remmy

#endif
