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
#include <cutils/io/writer_ref.hpp>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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

/// @brief -v's output: every removed path on a line of its own on stdout.
///
/// Buffered like rm's stdio stdout (flushed when full and at exit, and after
/// every line when stdout is a terminal), and shared by the workers under a
/// lock. A path is added after its removal succeeded, and a directory is only
/// removed once its contents are, so each line comes after the lines of what
/// the removal depended on: the lines of one directory keep its readdir order
/// and a directory follows its contents. Only the relative order of sibling
/// subtrees and of different operands, which remmy removes in parallel,
/// differs from rm's.
///
/// Whatever is left is written out on destruction. Write failures are ignored,
/// as rm ignores them. Without -v there is no log: the workers hold a null
/// pointer, and the removal path pays one branch for it.
class RemovedLog {
 public:
  RemovedLog() noexcept : line_buffered_(::isatty(STDOUT_FILENO) != 0) {
    buffer_.reserve(kCapacity);
  }

  RemovedLog(const RemovedLog&) = delete;
  auto operator=(const RemovedLog&) -> RemovedLog& = delete;
  RemovedLog(RemovedLog&&) = delete;
  auto operator=(RemovedLog&&) -> RemovedLog& = delete;

  ~RemovedLog() { Flush(); }

  /// @brief Adds the line made of `pieces` and a newline, leaving errno as it
  ///        was, so a caller may still report the error after it.
  auto Add(std::initializer_list<std::string_view> pieces) noexcept -> void {
    const int saved_errno = errno;
    {
      const std::lock_guard lock(mutex_);
      for (const std::string_view piece : pieces) {
        buffer_.append(piece);
      }
      buffer_.push_back('\n');
      if (line_buffered_ || buffer_.size() >= kCapacity) {
        FlushLocked();
      }
    }
    errno = saved_errno;
  }

  /// @brief Writes out what is buffered.
  auto Flush() noexcept -> void {
    const std::lock_guard lock(mutex_);
    FlushLocked();
  }

 private:
  static constexpr std::size_t kCapacity = std::size_t{64} * 1024;

  auto FlushLocked() noexcept -> void {
    std::string_view rest = buffer_;
    while (!rest.empty()) {
      const ssize_t written = ::write(STDOUT_FILENO, rest.data(), rest.size());
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      rest.remove_prefix(static_cast<std::size_t>(written));
    }
    buffer_.clear();
  }

  std::mutex mutex_;
  std::string buffer_;
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

/// @brief Whether an operand's failure is one to report: -f silences a missing
///        operand (ENOENT, which also covers "" and paths through missing
///        directories) and nothing else, as BSD rm does, except that with -r
///        or -R it also hides failures to stat (see StatFailureReportable).
auto Reportable(bool force, int error) noexcept -> bool {
  return !force || error != ENOENT;
}

/// @brief Whether an operand lstat(2) failed on with `error` is one to report.
///
/// Without -r or -R, -f silences only ENOENT (see Reportable). With either,
/// BSD rm hands its operands to fts(3) and, under -f and for anyone but root
/// (rm's `needstat`), passes over every operand fts could not stat without a
/// word, whatever the error: a trailing slash on a file, a path through a file
/// or an unsearchable directory, a symlink loop, a name that is too long. Root
/// gets rm's needstat path, which again hides only ENOENT.
auto StatFailureReportable(const remmy::Options& options, int error) noexcept
    -> bool {
  if (options.force && options.recursive && geteuid() != 0) {
    return false;
  }
  return Reportable(options.force, error);
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
          WarnAt(cli_.ExecutableName(), task->PathInto(path_buffer_),
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
        WarnAt(cli_.ExecutableName(), task->PathInto(path_buffer_),
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
            WarnAt(cli_.ExecutableName(), task->PathInto(path_buffer_),
                   entry.name(), errno);
          }
          continue;
        }
        is_dir = S_ISDIR(st.st_mode);
      }

      if (!is_dir) {
        if (cutils::os::unlinkat(task->fd_, entry.c_str(), 0) == 0) {
          if (removed_ != nullptr) {
            removed_->Add({dir_path, "/", entry.name()});
          }
        } else if (errno != ENOENT) {
          failures_++;
          WarnAt(cli_.ExecutableName(), task->PathInto(path_buffer_),
                 entry.name(), errno);
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
            WarnAt(cli_.ExecutableName(), task->PathInto(path_buffer_),
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

  /// @brief Passes a removal's `status` through, logging the removed path
  ///        (the concatenated `pieces`) under -v when it succeeded.
  ///
  /// Only for removals that are rare or cost a path anyway: the pieces are
  /// built whether or not -v is on.
  auto LogRemoval(int status,
                  std::initializer_list<std::string_view> pieces) noexcept
      -> int {
    if (status == 0 && removed_ != nullptr) {
      removed_->Add(pieces);
    }
    return status;
  }

  auto LogRemoval(int status, std::string_view path) noexcept -> int {
    return LogRemoval(status, {path});
  }

  auto LogRemoval(int status, std::string_view dir,
                  std::string_view name) noexcept -> int {
    return LogRemoval(status, {dir, "/", name});
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
        WarnAt(cli_.ExecutableName(), path_buffer_, errno);
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

/// @brief Whether the operand's last component is `.` or `..`.
constexpr auto IsDotOrDotDotOperand(std::string_view path) noexcept -> bool {
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }

  const std::size_t slash = path.rfind('/');
  const std::string_view last =
      slash == std::string_view::npos ? path : path.substr(slash + 1);

  return last == "." || last == "..";
}

/// @brief The operands left once BSD rm's guards have dropped theirs.
struct GuardedOperands {
  std::vector<const char*> kept;
  /// @brief Whether a guard dropped an operand, which makes rm exit 1.
  bool refused = false;
};

/// @brief BSD rm's checkdot(), then its checkslash().
///
/// Before anything is removed, rm drops every operand whose last component is
/// "." or "..", then every operand that is exactly "/" ("//" is an ordinary
/// directory), and says so once per guard for the whole command line, dot
/// first, whatever the options (-f included). The rest keep their order.
auto ApplyGuards(std::string_view prog,
                 std::span<const char* const> operands) noexcept
    -> GuardedOperands {
  GuardedOperands out;
  out.kept.reserve(operands.size());
  bool dot = false;
  bool slash = false;
  for (const char* path : operands) {
    if (IsDotOrDotDotOperand(path)) {
      dot = true;
    } else if (std::string_view(path) == "/") {
      slash = true;
    } else {
      out.kept.push_back(path);
    }
  }

  if (dot) {
    std::ignore =
        cutils::io::PrintLn(cutils::io::stderr_writer,
                            "{}: \".\" and \"..\" may not be removed", prog);
  }
  if (slash) {
    std::ignore = cutils::io::PrintLn(cutils::io::stderr_writer,
                                      "{}: \"/\" may not be removed", prog);
  }
  out.refused = dot || slash;
  return out;
}

/// @brief Whether rm runs as unlink(1): argv[0]'s last component, as given
///        (not getprogname(3)), is "unlink".
constexpr auto IsUnlinkMode(std::string_view argv0) noexcept -> bool {
  const std::size_t slash = argv0.rfind('/');
  return (slash == std::string_view::npos ? argv0 : argv0.substr(slash + 1)) ==
         "unlink";
}

/// @brief Whether BSD rm skips its guards for this command line.
///
/// Only unlink(1)'s one accepted shape, `unlink file` or `unlink -- file`,
/// goes straight to unlink(2) without them; there "." and "/" are directories
/// like any other. Every other unlink-mode command line is a usage error to
/// rm and removes nothing, so until remmy reports those the same way, it keeps
/// the guards for them: an option such as -r must never walk ".", ".." or "/".
constexpr auto SkipsGuards(std::string_view argv0, std::span<char* const> args,
                           std::size_t operand_count) noexcept -> bool {
  if (!IsUnlinkMode(argv0) || operand_count != 1) {
    return false;
  }
  // args excludes argv[0]: exactly the operand, or "--" and the operand.
  return args.size() == 1 ||
         (args.size() == 2 && std::string_view(args.front()) == "--");
}

/// @brief Asks BSD rm's -I question when it would; true to go ahead.
///
/// rm asks once when, among the operands that exist (lstat(2)) and passed
/// its guards (see ApplyGuards), there is a directory under -r or -R, or more
/// than three in all. Only the first character of each answer line counts;
/// anything but y or n asks again, and end of input declines.
auto ConfirmPromptOnce(std::span<const char* const> operands,
                       bool recursive) noexcept -> bool {
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

/// @brief Whether BSD rm's -x (with -r or -R) could keep anything of these
///        operands: only a walk crosses devices, so only when one of them is
///        a directory (lstat(2)) that rm would walk. The operands have passed
///        rm's guards (see ApplyGuards).
auto OneFileSystemWouldMatter(std::span<const char* const> operands) noexcept
    -> bool {
  return std::ranges::any_of(operands, [](const char* path) {
    struct stat path_stat;
    return cutils::os::lstat(path, &path_stat) == 0 &&
           S_ISDIR(path_stat.st_mode);
  });
}

auto SeedRoot(Scheduler& scheduler, cutils::os::Fd dirfd, std::string_view path)
    -> void {
  auto* task = new DirNode(std::move(dirfd), nullptr, std::string{path});
  if (!scheduler.Submit(task)) {
    delete task;
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::span<char* const> args(
      argv, static_cast<std::size_t>(argc > 0 ? argc : 0));
  const auto rest = args.empty() ? args : args.subspan(1);
  const auto cli = remmy::Argv{argc, argv}.TryParseOrAbort();
  if (!cli) {
    return cli.error();
  }

  const std::string_view prog = cli->ExecutableName();
  const GuardedOperands guarded =
      SkipsGuards(cli->ProgramName(), rest, cli->Operands().size())
          ? GuardedOperands{.kept = {cli->Operands().begin(),
                                     cli->Operands().end()}}
          : ApplyGuards(prog, cli->Operands());
  const std::span<const char* const> operands(guarded.kept);

  if (cli->Options().one_file_system && cli->Options().recursive &&
      OneFileSystemWouldMatter(operands)) {
    std::ignore = cutils::io::PrintLn(
        cutils::io::stderr_writer,
        "{}: -x: not supported yet; nothing was removed", cli->ProgramName());
    return 1;
  }

  if (cli->Options().prompt_once &&
      !ConfirmPromptOnce(operands, cli->Options().recursive)) {
    return 1;
  }

  const bool force = cli->Options().force;
  // Declared before the scheduler, so it outlives the workers holding it and
  // flushes once they are done.
  std::optional<RemovedLog> removed;
  if (cli->Options().verbose) {
    removed.emplace();
  }
  // -v: operands are logged exactly as typed, and walks start from them.
  const auto log_removed = [&removed](int status, const char* path) {
    if (status == 0 && removed) {
      removed->Add({path});
    }
    return status;
  };

  const std::uint16_t threads = ThreadCount();
  const FileUnlinkWorker prototype(*cli, removed ? &*removed : nullptr);
  Scheduler scheduler(threads, prototype);

  std::size_t failures = guarded.refused ? 1 : 0;
  for (const char* path : operands) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      if (const int error = errno;
          StatFailureReportable(cli->Options(), error)) {
        WarnAt(prog, path, error);
        ++failures;
      }
      continue;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
      if (log_removed(cutils::os::unlink(path), path) != 0) {
        if (const int error = errno; Reportable(force, error)) {
          WarnAt(prog, path, error);
          ++failures;
        }
      }
      continue;
    }

    if (!cli->Options().recursive) {
      if (cli->Options().dir) {
        // -d: rmdir(2) the operand as given, so "l/" removes the link's
        // target and "//" fails with EISDIR, and report errno like unlink.
        if (log_removed(cutils::os::rmdir(path), path) != 0) {
          if (const int error = errno; Reportable(force, error)) {
            WarnAt(prog, path, error);
            ++failures;
          }
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
