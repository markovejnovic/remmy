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
/// getdirentries64, or getdents64 on Linux) and:
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
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cutils/io/buffered_writer.hpp>
#include <cutils/io/print.hpp>
#include <cutils/io/stderr_writer.hpp>
#include <cutils/io/stdin_reader.hpp>
#include <cutils/io/stdout_writer.hpp>
#include <cutils/io/writer_ref.hpp>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/variant.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

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

class RemovedLog {
 public:
  RemovedLog() noexcept
      : out_({.shared = kCapacity, .handle = 0}, STDOUT_FILENO),
        line_buffered_(::isatty(STDOUT_FILENO) != 0) {}

  auto Add(std::string_view path) noexcept -> void {
    const std::array<std::string_view, 2> line{path, "\n"};
    Write(line);
  }

  auto Add(std::string_view dir, std::string_view name) noexcept -> void {
    const std::array<std::string_view, 4> line{dir, "/", name, "\n"};
    Write(line);
  }

 private:
  static constexpr std::size_t kCapacity = 64 * 1024;

  auto Write(std::span<const std::string_view> line) noexcept -> void {
    std::ignore = out_.WriteMany(line);
    if (line_buffered_) {
      std::ignore = out_.Flush();
    }
  }

  cutils::io::StdoutWriter out_;
  bool line_buffered_;
};

/// @brief Report a failure the same way BSD rm's does.
auto WarnAt(std::string_view prog, std::string_view path, int error) noexcept
    -> void {
  std::ignore = cutils::io::PrintLn(cutils::io::stderr_writer, "{}: {}: {}",
                                    prog, path, cutils::os::StrError(error));
}

/// @brief WarnAt for the entry `name` of the directory at `dir`.
auto WarnAt(std::string_view prog, std::string_view dir, std::string_view name,
            int error) noexcept -> void {
  std::ignore =
      cutils::io::PrintLn(cutils::io::stderr_writer, "{}: {}/{}: {}", prog, dir,
                          name, cutils::os::StrError(error));
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

  /// @brief Create a worker.
  /// @note Cli must outlive the worker.
  explicit FileUnlinkWorker(const remmy::Cli& cli, RemovedLog* removed) noexcept
      : cli_(cli), removed_(removed) {}

  /// @brief The total number of failures that this worker encountered.
  [[nodiscard]] auto Failures() const noexcept -> std::size_t {
    return failures_;
  }

  /// @brief The main entry-point the scheduler invokes for this task.
  ///
  /// This function is called by the scheduler periodically as new tasks are
  /// admitted into the scheduler.
  void Process(DirNode* task, auto& ctx) noexcept {
    auto open_result = task->Open(path_buffer_);
    if (!open_result) {
      const cutils::os::OpenError err = open_result.error();
      if (err.retryable) {
        // Out of descriptors, but one of ours is or will be freed: park the
        // directory and retry it later.
        ctx.Submit(task, kAwaitingDescriptor);
      } else {
        if (!cli_.Options().force ||
            (RemoveEmptyLogged(task) != 0 && errno != ENOENT)) {
          failures_++;
          WarnAt(cli_.CommandName(), task->PathInto(path_buffer_),
                 static_cast<int>(err.code));
        }

        DirNode* parent = task->parent_;
        delete task;
        if (parent != nullptr) {
          MaybeCleanupDirNode(parent);
        }
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
    // Entries are logged as "<dir>/<name>", fts's path for them.
    const std::string_view dir_path =
        removed_ != nullptr ? task->PathInto(scan_path_) : "";
    for (const auto& read : dirs_.Read(task->fd_)) {
      if (!read) {
        // A failed read is not the end of the directory; say so and stop.
        failures_++;
        WarnAt(cli_.CommandName(), task->PathInto(path_buffer_),
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
            failures_++;
            WarnAt(cli_.CommandName(), task->PathInto(path_buffer_),
                   entry.name(), errno);
          }
          continue;
        }
        is_dir = S_ISDIR(st.st_mode);
      }

      if (!is_dir) {
        if (cutils::os::unlinkat(task->fd_, entry.c_str(), 0) == 0) {
          if (removed_ != nullptr) {
            removed_->Add(dir_path, entry.name());
          }
        } else if (errno != ENOENT) {
          failures_++;
          WarnAt(cli_.CommandName(), task->PathInto(path_buffer_), entry.name(),
                 errno);
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
          if (!cli_.Options().force ||
              (LogRemoval(
                   cutils::os::unlinkat(task->fd_, entry.c_str(), AT_REMOVEDIR),
                   dir_path, entry.name()) != 0 &&
               errno != ENOENT)) {
            failures_++;
            WarnAt(cli_.CommandName(), task->PathInto(path_buffer_),
                   entry.name(), static_cast<int>(opened.error().code));
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

  auto LogRemoval(int status, std::string_view path) noexcept -> int {
    if (status == 0 && removed_ != nullptr) {
      removed_->Add(path);
    }
    return status;
  }

  auto LogRemoval(int status, std::string_view dir,
                  std::string_view name) noexcept -> int {
    if (status == 0 && removed_ != nullptr) {
      removed_->Add(dir, name);
    }
    return status;
  }

  auto RemoveEmptyLogged(const DirNode* node) noexcept -> int {
    const int status = node->RemoveEmpty(path_buffer_);
    return LogRemoval(status, path_buffer_);
  }

  /// @brief Cleanup a DirNode if we need to.
  ///
  /// This tries to delete a DirNode if there are no more DirNode's referencing
  /// the given one. It cleans up its parents equivalently.
  void MaybeCleanupDirNode(DirNode* node) noexcept {
    const auto chain = node->ParentsMut();

    for (auto it = chain.begin(); it != chain.end();) {
      DirNode* current = *it;
      ++it;

      if (current->remaining_children_dirs_.fetch_sub(
              1, std::memory_order_release) != 1) {
        return;
      }
      std::atomic_thread_fence(std::memory_order_acquire);

      if (RemoveEmptyLogged(current) != 0 && errno != ENOENT) {
        failures_++;
        WarnAt(cli_.CommandName(), path_buffer_, errno);
      }

      delete current;
    }
  }

  /// @brief The command line this run removes for.
  const remmy::Cli& cli_;

  /// @brief Thread-local buffer used to compute the abspath of DirNode.
  std::string path_buffer_;

  /// @brief Thread-local iterator used to iterate over directories. Reset on
  ///        each new task.
  cutils::os::DirReader dirs_;

  /// @brief The total number of failures that this worker encountered.
  std::size_t failures_ = 0;

  /// @brief -v: where removed paths go; null without -v.
  RemovedLog* removed_;

  /// @brief Thread-local buffer holding the path of the directory being
  ///        scanned, for -v's lines about its entries.
  std::string scan_path_;
};

using Scheduler =
    cutils::TaskScheduler<FileUnlinkWorker, FileUnlinkWorker::kRanks>;

/// @brief Asks BSD rm's -I question when it would; true to go ahead.
///
/// rm asks once when, among the operands that exist (lstat(2)), there is a
/// directory under -r or -R, or more than three in all. Only the first
/// character of each answer line counts; anything but y or n asks again, and
/// end of input declines.
auto ConfirmPromptOnce(std::span<char* const> operands, bool recursive) noexcept
    -> bool {
  static constexpr std::size_t kMaxSilentOperands = 3;
  static constexpr std::size_t kPromptBuffer = 256;
  auto& in = cutils::io::stdin_reader;

  std::size_t dirs = 0;
  std::size_t files = 0;
  std::string_view dir_name;
  for (const char* path : operands) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      continue;
    }
    if (S_ISDIR(path_stat.st_mode)) {
      ++dirs;
      dir_name = path;
    } else {
      ++files;
    }
  }

  const bool ask_recursive = recursive && dirs > 0;
  if (!ask_recursive && dirs + files <= kMaxSilentOperands) {
    return true;
  }

  while (true) {
    cutils::io::InlineBufferedWriter<
        cutils::io::WriterRef<decltype(cutils::io::stderr_writer)>,
        kPromptBuffer>
        line(cutils::io::stderr_writer);
    if (ask_recursive) {
      std::ignore = cutils::io::Print(line, "recursively remove");
      if (dirs == 1) {
        std::ignore = cutils::io::Print(line, " {}", dir_name);
      } else {
        std::ignore = cutils::io::Print(line, " {} dirs", dirs);
      }
      if (files == 1) {
        std::ignore = cutils::io::Print(line, " and 1 file");
      } else if (files > 1) {
        std::ignore = cutils::io::Print(line, " and {} files", files);
      }
    } else {
      std::ignore = cutils::io::Print(line, "remove {} files", dirs + files);
    }
    std::ignore = cutils::io::Print(line, "? ");
    std::ignore = line.Flush();

    const std::optional<char> first = in.ReadByte();
    const bool line_ended = first == '\n' || (first && in.SkipPast('\n'));

    if (first == 'y' || first == 'Y') {
      return true;
    }
    if (first == 'n' || first == 'N' || !line_ended) {
      return false;
    }
  }
}

/// @brief Run as unlink(1).
///
/// BSD rm has this mode wherein it special-cases its behavior based on how its
/// invoked -- if it's called `unlink` via the CLI, it takes this behavior.
///
/// It pretty much forwards the call straight to the unlink syscall.
auto RunUnlink(const remmy::UnlinkCli& cli) noexcept -> int {
  const char* path = cli.Operand();
  const std::string_view prog = cli.CommandName();

  struct stat path_stat;
  if (cutils::os::lstat(path, &path_stat) != 0) {
    WarnAt(prog, path, errno);
    return 1;
  }

  if (S_ISDIR(path_stat.st_mode)) {
    std::ignore = cutils::io::PrintLn(cutils::io::stderr_writer,
                                      "{}: {}: is a directory", prog, path);
    return 1;
  }

  if (cutils::os::unlink(path) != 0) {
    WarnAt(prog, path, errno);
    return 1;
  }

  return 0;
}

auto SeedRoot(Scheduler& scheduler, cutils::os::Fd dirfd, std::string_view path)
    -> void {
  auto* task = new DirNode(std::move(dirfd), nullptr, std::string{path});
  if (!scheduler.Submit(task)) {
    delete task;
  }
}

auto RunRm(const remmy::Cli& cli) -> int {
  if (cli.Options().prompt_once &&
      !ConfirmPromptOnce(cli.Operands(), cli.Options().recursive)) {
    return 1;
  }

  const bool force = cli.Options().force;
  // Declared before the scheduler, so it outlives the workers holding it and
  // flushes once they are done.
  std::optional<RemovedLog> removed;
  if (cli.Options().verbose) {
    removed.emplace();
  }
  // -v: operands are logged exactly as typed, and walks start from them.
  const auto log_removed = [&removed](int status, const char* path) {
    if (status == 0 && removed) {
      removed->Add(path);
    }
    return status;
  };

  const std::uint16_t threads = ThreadCount();
  const std::string_view prog = cli.CommandName();
  const FileUnlinkWorker prototype(cli, removed ? &*removed : nullptr);
  Scheduler scheduler(threads, prototype);

  std::size_t failures = cli.HasDroppedOperands() ? 1 : 0;
  for (const char* path : cli.Operands()) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      if (const int error = errno;
          !(force && cli.Options().recursive && cutils::os::GetEUid() != 0) &&
          (!force || error != ENOENT)) {
        WarnAt(prog, path, error);
        ++failures;
      }
      continue;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
      if (log_removed(cutils::os::unlink(path), path) != 0) {
        if (const int error = errno; !force || error != ENOENT) {
          WarnAt(prog, path, error);
          ++failures;
        }
      }
      continue;
    }

    if (!cli.Options().recursive) {
      if (cli.Options().dir) {
        if (log_removed(cutils::os::rmdir(path), path) != 0 &&
            (!force || errno != ENOENT)) {
          WarnAt(prog, path, errno);
          ++failures;
        }

        continue;
      }

      std::ignore = cutils::io::PrintLn(cutils::io::stderr_writer,
                                        "{}: {}: is a directory", prog, path);
      ++failures;
      continue;
    }

    auto dirfd =
        cutils::os::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dirfd) {
      if (!force || (log_removed(cutils::os::rmdir(path), path) != 0 &&
                     errno != ENOENT)) {
        WarnAt(prog, path, static_cast<int>(dirfd.error().code));
        ++failures;
      }
      continue;
    }

    // Seeding precedes Run, so this is still single-threaded.
    SeedRoot(scheduler, *std::move(dirfd), path);
  }

  (void)scheduler.Wait();

  for (const auto& worker : scheduler.Workers()) {
    failures += worker.Failures();
  }

  return failures == 0 ? 0 : 1;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const auto parsed = remmy::Argv{argc, argv}.TryParseOrAbort();
  if (!parsed) {
    return parsed.error();
  }

  return std::visit(
      cutils::variant::Overloaded{
          [](const remmy::UnlinkCli& cli) { return RunUnlink(cli); },
          [](const remmy::Cli& cli) { return RunRm(cli); },
      },
      *parsed);
}
