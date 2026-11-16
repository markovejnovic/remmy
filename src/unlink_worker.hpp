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
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cutils/os/attr_dir_reader.hpp"
#include "cutils/os/fd.hpp"
#include "cutils/os/limits/fd.hpp"
#include "cutils/os/os.hpp"
#include "dir_node.hpp"
#include "prompt.hpp"
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
      : remover_(std::move(remover)), attrs_(MakeAttrs(remover_.Rules())) {}

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
  /// @brief Stands in for a member a policy does not need.
  struct Nothing {};

  [[nodiscard]] static auto MakeAttrs(const Policy& policy) {
    if constexpr (Policy::kReadsAttrs) {
      return cutils::os::AttrDirReader(policy.ReadMethod());
    } else {
      return Nothing{};
    }
  }

  void ProcessTask(DirNode* task, auto& ctx) noexcept {
    // A run that asks opens a queued directory only by its recorded chain.
    auto open_result =
        Policy::kPrompts ? task->OpenChecked() : task->Open(path_buffer_);
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

  /// @brief One entry of a directory being scanned.
  struct Listed {
    std::string_view name;

    /// @brief `name`, NUL-terminated.
    const char* c_name;
    bool is_dir;

    /// @brief Whether rm's override prompt comes before removing it.
    bool needs_override;
  };

  /// @brief Scan through the given directory.
  void Scan(DirNode* task, auto& ctx) noexcept {
    // Entries are logged as "<dir>/<name>", fts's path for them.
    const std::string_view dir_path =
        remover_.Command().Options().verbose ? task->PathInto(scan_path_) : "";
    if constexpr (Policy::kReadsAttrs) {
      ScanWithAttrs(task, dir_path, ctx);
    } else {
      ScanListing(task, dir_path, ctx);
    }
  }

  void ScanListing(DirNode* task, std::string_view dir_path,
                   auto& ctx) noexcept {
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

      Visit(task,
            Listed{.name = entry.name(),
                   .c_name = entry.c_str(),
                   .is_dir = is_dir,
                   .needs_override = false},
            dir_path, ctx);
    }
  }

  void ScanWithAttrs(DirNode* task, std::string_view dir_path,
                     auto& ctx) noexcept {
    const auto read =
        attrs_.ForEach(task->fd_, [&](const cutils::os::AttrEntry& entry) {
          if (entry.error() != 0) {
            if (entry.error() != ENOENT) {
              remover_.Fail(task->PathInto(path_buffer_), entry.name(),
                            entry.error());
            }
            return;
          }
          Visit(task,
                Listed{
                    .name = entry.name(),
                    .c_name = entry.c_str(),
                    .is_dir = entry.kind() == cutils::os::EntryKind::kDirectory,
                    .needs_override = remover_.Rules().NeedsOverride(
                        entry.kind(), entry.writable(), entry.flags())},
                dir_path, ctx);
        });
    if (!read) {
      // A failed read is not the end of the directory; say so and stop.
      remover_.Fail(task->PathInto(path_buffer_),
                    static_cast<int>(read.error()));
    }
  }

  /// @brief Remove, schedule or ask about one entry of `task`.
  void Visit(DirNode* task, const Listed& entry, std::string_view dir_path,
             auto& ctx) noexcept {
    if constexpr (Policy::kAsksEveryEntry) {
      Ask(task, entry, ctx);
      return;
    }

    if (entry.is_dir && entry.needs_override &&
        remover_.Rules().AsksBeforeEntering()) {
      AskToEnter(task, entry, ctx);
      return;
    }

    if (!entry.is_dir) {
      if (entry.needs_override) {
        AskOverride(task, entry, ctx);
        return;
      }
      if (remover_.Logged(cutils::os::unlinkat(task->fd_, entry.c_name, 0),
                          "{}/{}", dir_path, entry.name) != 0 &&
          errno != ENOENT) {
        remover_.Fail(task->PathInto(path_buffer_), entry.name, errno);
      }
      return;
    }

    // Near the ceiling, skip the attempt and park the child straight away.
    cutils::os::Fd child_fd;
    if (!cutils::os::limits::fd::Pool::Exhausted()) {
      auto opened =
          cutils::os::openat(task->fd_, entry.c_name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (opened) {
        child_fd = *std::move(opened);
      } else if (!opened.error().retryable) {
        if (!remover_.Command().Options().force ||
            (remover_.Logged(
                 cutils::os::unlinkat(task->fd_, entry.c_name, AT_REMOVEDIR),
                 "{}/{}", dir_path, entry.name) != 0 &&
             errno != ENOENT)) {
          remover_.Fail(task->PathInto(path_buffer_), entry.name,
                        static_cast<int>(opened.error().code));
        }
        return;
      }
    }

    // Count the child on the parent before publishing it: a worker could
    // steal and finish it the instant Submit returns, and the parent's own
    // scan reference (held until Finish, below) keeps
    // `remaining_children_dirs_` from reaching zero mid-scan regardless.
    auto* child =
        new DirNode(std::move(child_fd), task, std::string{entry.name});
    child->needs_override_ = entry.needs_override;
    if constexpr (Policy::kPrompts) {
      if (!Record(task, entry, *child)) {
        delete child;
        return;
      }
    }
    task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t tier =
        child->fd_.IsOpen() ? kRunnable : kAwaitingDescriptor;

    // The scheduler carries raw pointers (its slots are trivially copyable
    // and a steal may briefly duplicate one), so ownership is manual: the
    // node is freed by whichever worker drops its last reference in Finish.
    ctx.Submit(child, tier);
  }

  /// @brief Hand a write-protected file to the prompter, with the stat its
  ///        question prints, read while `task` is still open.
  void AskOverride(DirNode* task, const Listed& entry, auto& ctx) noexcept {
    struct stat st;
    if (cutils::os::fstatat(task->fd_, entry.c_name, &st,
                            AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT) {
        remover_.Fail(task->PathInto(path_buffer_), entry.name, errno);
      }
      return;
    }
    // Counted on the directory first, so it outlives the question.
    task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
    remover_.Requests().Push(ConfirmOverride{.parent = task,
                                             .name = std::string{entry.name},
                                             .info = EntryStat::From(st),
                                             .token = ctx.Hold()});
  }

  /// @brief Hand an entry of `task` to the prompter instead of removing or
  ///        entering it.
  void Ask(DirNode* task, const Listed& entry, auto& ctx) noexcept {
    if (!entry.is_dir) {
      // Counted on the directory first, as for a scheduled child, so it
      // outlives the question.
      task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
      remover_.Requests().Push(ConfirmRemove{.parent = task,
                                             .name = std::string{entry.name},
                                             .token = ctx.Hold()});
      return;
    }
    AskToEnter(task, entry, ctx);
  }

  /// @brief Hand the directory `entry` of `task` to the prompter before
  ///        entering it.
  void AskToEnter(DirNode* task, const Listed& entry, auto& ctx) noexcept {
    // Not opened: a question may wait a long time, and must not hold a
    // descriptor while it does. Process opens it once answered, through
    // the identity recorded here.
    auto* child = new DirNode(cutils::os::Fd{}, task, std::string{entry.name});
    child->needs_override_ = entry.needs_override;
    if (!Record(task, entry, *child)) {
      delete child;
      return;
    }
    task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
    remover_.Requests().Push(ConfirmExamine{.dir = child, .token = ctx.Hold()});
  }

  /// @brief Give `child`, the directory `entry` of `task`, its stat_: from
  ///        its open descriptor if it has one, else from `task`'s.
  ///
  /// False, having reported anything but ENOENT, when it cannot be read.
  [[nodiscard]] auto Record(DirNode* task, const Listed& entry,
                            DirNode& child) noexcept -> bool {
    struct stat st;
    const int status = child.fd_.IsOpen()
                           ? ::fstat(child.fd_.get(), &st)
                           : cutils::os::fstatat(task->fd_, entry.c_name, &st,
                                                 AT_SYMLINK_NOFOLLOW);
    if (status != 0) {
      if (errno != ENOENT) {
        remover_.Fail(task->PathInto(path_buffer_), entry.name, errno);
      }
      return false;
    }
    child.stat_ = std::make_unique<EntryStat>(EntryStat::From(st));
    return true;
  }

  Remover<Policy> remover_;

  /// @brief Thread-local buffer used to compute the abspath of DirNode.
  std::string path_buffer_;

  /// @brief Thread-local iterator used to iterate over directories. Reset on
  ///        each new task.
  cutils::os::DirReader dirs_;

  /// @brief The listing that also reads attributes, for policies that need
  ///        them; nothing otherwise.
  [[no_unique_address]] std::conditional_t<
      Policy::kReadsAttrs, cutils::os::AttrDirReader, Nothing> attrs_;

  /// @brief Thread-local buffer holding the path of the directory being
  ///        scanned, for -v's lines about its entries.
  std::string scan_path_;
};

}  // namespace remmy

#endif
