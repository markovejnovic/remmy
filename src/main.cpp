// See LICENSE in the repository root.

/// Remmy is `rm` that's a lot faster on macOS.
///
/// It's pretty much as fast I could make `rm` go. If you manage to make a
/// faster `rm`, **please** contact me. I would love to see it =)
///
/// The general algorithm is here described.
///
/// The program starts off in single-threaded mode. For all positional paths
/// given in the input CLI, this program stats them, and, if it is a file,
/// deletes it. If the path is a directory, however, this program opens the
/// directory and emplaces the new open FD in a multi-threaded scheduler.
///
/// This is where the fun begins. The multi-threaded scheduler schedules a
/// worker for each available thread. The total number of threads remmy uses is
/// far smaller than the total available threads on your machine. I've profiled
/// peak performance to be at around 4 threads.
///
/// Each pushed directory gets consumed by a worker. Each worker then walks
/// through
/// the directory (without statting this time around, rather relying on
/// getdirentries64) and:
///
///   - For each file, it `unlink`s it.
///   - For each directory, it opens the directory and pushes it back into the
///     scheduler for another worker to pick up on it.
///
/// There are more caveats here that I'll briefly bore you with:
///
///   - Most POSIX systems have a limit on the total number of open directories,
///     so an "open" directory, in reality, might be a directory marked as
///     non-openable and deleted via a path.
///   - I have avoided describing how directories are deleted -- if a directory
///     is opened and it has children directories, it is necessary to wait for
///     the children directories to be deleted. To keep logic simple, remmy
///     holds a refcount on the parent node, and doesn't delete it if the
///     refcount isn't zero. Each active child increments the refcount by one.
///
///     This poses a strict penalty on children, each child's worker needs to
///     check whether it needs to cleanup the parent or not, so there is some
///     duplicate work there, but it is often one pointer jump and just an
///     atomic read.
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cutils/clppap/report.hpp>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "cli.hpp"
#include "dir_node.hpp"

namespace {

using remmy::DirNode;

static auto ThreadCount() -> std::uint16_t {
  static constexpr const char* kEnvThreadsName = "REMMY_THREADS";
  static constexpr unsigned kMaxThreads = 4;

  const auto n = cutils::GetEnvOrDefault<std::uint16_t>(kEnvThreadsName, 0);
  if (n > 0) {
    return n;
  }

  const unsigned hw = std::thread::hardware_concurrency();
  return static_cast<std::uint16_t>(std::min(hw, kMaxThreads));
}

/// @brief Traverses directory, unlinks files, schedules subdirs as tasks.
///
/// Do note that this type is **stateful** across multiple tasks. The scheduler
/// I've written will inject new tasks via [`Process`].
class FileUnlinkWorker {
 public:
  using task_type = DirNode*;

  static constexpr std::size_t kRunnable = 0;
  static constexpr std::size_t kAwaitingDescriptor = 1;
  static constexpr std::size_t kRanks = 2;

  /// @brief Thread-local buffer used to compute the abspath of DirNode.
  std::string path_buffer_;

  /// @brief Thread-local iterator used to iterate over directories. Reset on
  ///        each new task.
  cutils::os::DirReader dirs_;

  /// @brief The total number of failures that this worker encountered.
  std::size_t failures_ = 0;

  /// @brief The main entry-point the scheduler invokes for this task.
  ///
  /// This function is called by the scheduler periodically as new tasks are
  /// admitted into the scheduler.
  void Process(DirNode* task, auto& ctx) noexcept {
    auto open_result = task->Open(path_buffer_);
    if (!open_result) {
      const std::errc err = open_result.error();
      // We've failed to open the path_buffer_. If it's due to exhausted FDs,
      // and we have FDs we can open, let's reschedule it and hope that later
      // we'll have some free FDs to use.
      if ((err == std::errc::too_many_files_open_in_system ||
           err == std::errc::too_many_files_open) &&
          cutils::os::limits::fd::Pool::Live() > 0) {
        ctx.Submit(task, kAwaitingDescriptor);
      } else {
        // We're not using any FDs, so chances are we won't be able to get
        // anywhere.
        //
        // Note there's a small TOCTOU race here -- when we "note exhaustion"
        // inside of DirNode::Open, we _could_ have it return the exhaust FD
        // count we're using to avoid the TOCTOU race, but it's probably fine.
        failures_++;
        std::println(stderr, "cannot open '{}': {}",
                     task->PathInto(path_buffer_),
                     std::strerror(static_cast<int>(err)));
        MaybeCleanupDirNode(task);
      }

      return;
    }

    Scan(task, ctx);
    task->fd_.Close();

    MaybeCleanupDirNode(task);
  }

 private:
  /// @brief Scan through the given directory.
  void Scan(DirNode* task, auto& ctx) noexcept {
    for (const auto& read : dirs_.Read(task->fd_)) {
      if (!read) {
        // A failed read is not the end of the directory; say so and stop.
        failures_++;
        std::println(stderr, "cannot read '{}': {}",
                     task->PathInto(path_buffer_),
                     std::strerror(static_cast<int>(read.error())));
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
                                AT_SYMLINK_NOFOLLOW) == 0) {
          is_dir = S_ISDIR(st.st_mode);
        }
      }

      if (!is_dir) {
        // ENOENT means it is already gone, which is all rm -f asks for.
        if (cutils::os::unlinkat(task->fd_, entry.c_str(), 0) != 0 &&
            errno != ENOENT) {
          failures_++;
          std::println(stderr, "cannot remove '{}/{}': {}",
                       task->PathInto(path_buffer_), entry.name(),
                       std::strerror(errno));
        }
        continue;
      }

      const bool exhausted = cutils::os::limits::fd::Pool::Exhausted();
      cutils::os::Fd child_fd =
          exhausted ? cutils::os::Fd{}
                    : cutils::os::openat(
                          task->fd_, entry.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!child_fd.IsOpen() && !exhausted) {
        if (!(errno == EMFILE || errno == ENFILE)) {
          failures_++;
          std::println(stderr, "cannot open '{}/{}': {}",
                       task->PathInto(path_buffer_), entry.name(),
                       std::strerror(errno));
          continue;
        }
        cutils::os::limits::fd::Pool::NoteExhaustion();
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

  /// @brief Cleanup a DirNode if we need to.
  ///
  /// This tries to delete a DirNode if there are no more DirNode's referencing
  /// the given one. It cleans up its parents equivalently.
  void MaybeCleanupDirNode(DirNode* node) noexcept {
    auto chain = node->ParentsMut();

    for (auto it = chain.begin(); it != chain.end();) {
      DirNode* current = *it;
      ++it;

      if (current->remaining_children_dirs_.fetch_sub(
              1, std::memory_order_release) != 1) {
        return;
      }
      std::atomic_thread_fence(std::memory_order_acquire);

      const char* path = current->PathInto(path_buffer_);
      if (cutils::os::rmdir(path) != 0 && errno != ENOENT) {
        failures_++;
        std::println(stderr, "cannot remove '{}': {}", path,
                     std::strerror(errno));
      }

      delete current;
    }
  }
};

using Scheduler =
    cutils::TaskScheduler<FileUnlinkWorker, FileUnlinkWorker::kRanks>;

auto SeedRoot(Scheduler& scheduler, cutils::os::Fd dirfd, std::string_view path)
    -> void {
  auto* task = new DirNode(std::move(dirfd), nullptr, std::string{path});
  if (!scheduler.Submit(task)) {
    delete task;
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const auto cli_opts = cpplap::ParseOrReport<remmy::Cli>(argc, argv);
  if (!cli_opts) {
    return cli_opts.error();
  }

  const std::uint16_t threads = ThreadCount();
  Scheduler scheduler(threads);

  std::size_t failures = 0;
  for (const char* path : cli_opts->positional) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      std::println(stderr, "cannot remove '{}': {}", path,
                   std::strerror(errno));
      ++failures;
      continue;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
      if (cutils::os::unlink(path) != 0) {
        std::println(stderr, "cannot remove '{}': {}", path,
                     std::strerror(errno));
        ++failures;
      }
      continue;
    }

    if (!cli_opts->is_recursive) {
      std::println(stderr, "cannot remove '{}': {}", path,
                   std::strerror(EISDIR));
      ++failures;
      continue;
    }

    cutils::os::Fd dirfd =
        cutils::os::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dirfd.IsOpen()) {
      std::println(stderr, "cannot open '{}': {}", path, std::strerror(errno));
      ++failures;
      continue;
    }

    // Seeding precedes Run, so this is still single-threaded.
    SeedRoot(scheduler, std::move(dirfd), path);
  }

  (void)scheduler.Wait();

  for (const auto& worker : scheduler.Workers()) {
    failures += worker.failures_;
  }

  return failures == 0 ? 0 : 1;
}
