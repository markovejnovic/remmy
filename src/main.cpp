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
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

/// @brief Writes the pieces to stderr in one writev(2), ignoring failures.
///
/// Unlike std::print, this cannot throw (which under -fno-exceptions would
/// abort) when stderr is closed or full: rm itself exits normally then.
auto WriteStderr(std::initializer_list<std::string_view> pieces) noexcept
    -> void {
  static constexpr std::size_t kMaxPieces = 8;
  std::array<iovec, kMaxPieces> iov{};
  std::size_t count = 0;
  for (const std::string_view piece : pieces) {
    if (count == iov.size()) {
      break;
    }
    // writev only reads the buffers; iov_base is merely declared mutable.
    iov[count++] = iovec{.iov_base = const_cast<char*>(piece.data()),
                         .iov_len = piece.size()};
  }
  while (::writev(STDERR_FILENO, iov.data(), static_cast<int>(count)) < 0 &&
         errno == EINTR) {
  }
}

/// @brief The name rm's diagnostics start with: getprogname(3), which is the
///        basename of the executed file (a symlink's own name), not argv[0].
auto ProgramName() noexcept -> std::string_view {
  const char* name = ::getprogname();
  return name != nullptr ? name : "rm";
}

/// @brief strerror(3)'s text, held in a buffer of its own so that workers can
///        report errors concurrently (strerror_r(3); an unknown number still
///        gets rm's "Unknown error: N").
class ErrorText {
 public:
  explicit ErrorText(int error) noexcept {
    (void)::strerror_r(error, text_.data(), text_.size());
  }

  [[nodiscard]] auto View() const noexcept -> std::string_view {
    return text_.data();
  }

 private:
  static constexpr std::size_t kSize = 128;
  std::array<char, kSize> text_{};
};

/// @brief Reports a failure the way BSD rm's warn(3) does:
///        "<prog>: <path>: <strerror>", with `path` printed as given.
auto ReportError(std::string_view path, int error) noexcept -> void {
  const ErrorText text(error);
  WriteStderr({ProgramName(), ": ", path, ": ", text.View(), "\n"});
}

/// @brief ReportError for the entry `name` of the directory at `dir`, which is
///        the path fts(3) gives rm for it.
auto ReportError(std::string_view dir, std::string_view name,
                 int error) noexcept -> void {
  const ErrorText text(error);
  WriteStderr({ProgramName(), ": ", dir, "/", name, ": ", text.View(), "\n"});
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

/// @brief Whether a directory is gone after rmdir(2) (or unlinkat(2) with
///        AT_REMOVEDIR) returned `status`: removed now or already missing.
auto DirGone(int status) noexcept -> bool {
  return status == 0 || errno == ENOENT;
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

  /// @brief -f: like rm, try to rmdir a directory that cannot be opened (an
  ///        unreadable one, fts's FTS_DNR) and say nothing when that works.
  bool force_ = false;

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
        // Like the inline openat failure in Scan: report the directory and
        // leave it be (-f first tries rmdir, as rm does). It was never
        // scanned, so nothing references it; only its parent's count of it
        // is dropped.
        const char* path = task->PathInto(path_buffer_);
        if (!force_ || !DirGone(cutils::os::rmdir(path))) {
          failures_++;
          ReportError(path, static_cast<int>(err.code));
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
    for (const auto& read : dirs_.Read(task->fd_)) {
      if (!read) {
        // A failed read is not the end of the directory; say so and stop.
        failures_++;
        ReportError(task->PathInto(path_buffer_),
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
            ReportError(task->PathInto(path_buffer_), entry.name(), errno);
          }
          continue;
        }
        is_dir = S_ISDIR(st.st_mode);
      }

      if (!is_dir) {
        if (cutils::os::unlinkat(task->fd_, entry.c_str(), 0) != 0 &&
            errno != ENOENT) {
          failures_++;
          ReportError(task->PathInto(path_buffer_), entry.name(), errno);
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
          if (!force_ || !DirGone(cutils::os::unlinkat(task->fd_, entry.c_str(),
                                                       AT_REMOVEDIR))) {
            failures_++;
            ReportError(task->PathInto(path_buffer_), entry.name(),
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
        ReportError(path, errno);
      }

      delete current;
    }
  }
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
auto ApplyGuards(std::span<const char* const> operands) noexcept
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
    WriteStderr({ProgramName(), ": \".\" and \"..\" may not be removed\n"});
  }
  if (slash) {
    WriteStderr({ProgramName(), ": \"/\" may not be removed\n"});
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

/// @brief Prints BSD rm's answer to a rejected command line; returns 64.
///
/// Like getopt(3), the illegal-option line names argv[0] exactly as given.
auto ReportUsage(std::string_view argv0, remmy::UsageError error) noexcept
    -> int {
  if (error.illegal_option != '\0') {
    WriteStderr({argv0, ": illegal option -- ",
                 std::string_view(&error.illegal_option, 1), "\n",
                 remmy::kUsage});
  } else {
    WriteStderr({remmy::kUsage});
  }
  return remmy::kExitUsage;
}

/// @brief Whether BSD rm's -I would ask before removing these operands.
///
/// rm asks once when, among the operands that exist (lstat(2)) and passed
/// its guards (see ApplyGuards), there is a directory under -r or -R, or more
/// than three in all. Otherwise -I changes nothing.
auto PromptOnceWouldAsk(std::span<const char* const> operands,
                        bool recursive) noexcept -> bool {
  static constexpr std::size_t kMaxSilentOperands = 3;
  std::size_t existing = 0;
  for (const char* path : operands) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      continue;
    }
    if (recursive && S_ISDIR(path_stat.st_mode)) {
      return true;
    }
    ++existing;
  }
  return existing > kMaxSilentOperands;
}

/// @brief Whether BSD rm's -i would ask before removing any of these operands.
///
/// An effective -i (so not one a later -f overrode) asks before every removal,
/// but rm reports an operand that is missing (lstat(2)) or a directory without
/// -r, -R or -d straight away, without asking. The operands have passed rm's
/// guards (see ApplyGuards), which never ask either.
auto InteractiveWouldAsk(std::span<const char* const> operands,
                         const remmy::Options& options) noexcept -> bool {
  const bool removes_dirs = options.recursive || options.dir;
  return std::ranges::any_of(operands, [removes_dirs](const char* path) {
    struct stat path_stat;
    return cutils::os::lstat(path, &path_stat) == 0 &&
           (removes_dirs || !S_ISDIR(path_stat.st_mode));
  });
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

/// @brief Refuses a command line remmy cannot yet honour safely; returns 1.
///
/// Ignoring -i, -I, -W or -x would remove what rm would ask about or keep, so
/// nothing is touched instead.
auto ReportUnsupported(std::string_view argv0, char option) noexcept -> int {
  WriteStderr({argv0, ": -", std::string_view(&option, 1),
               ": not supported yet; nothing was removed\n"});
  return 1;
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
  const char* argv0 = args.empty() ? "rm" : args.front();
  const auto rest = args.empty() ? args : args.subspan(1);
  const auto cli = remmy::ParseCli(rest);
  if (!cli) {
    return ReportUsage(argv0, cli.error());
  }
  const GuardedOperands guarded =
      SkipsGuards(argv0, rest, cli->operands.size())
          ? GuardedOperands{.kept = {cli->operands.begin(),
                                     cli->operands.end()}}
          : ApplyGuards(cli->operands);
  const std::span<const char* const> operands(guarded.kept);
  if (!operands.empty()) {
    if (const char option = remmy::UnsupportedOption(cli->options);
        option != '\0') {
      return ReportUnsupported(argv0, option);
    }
    if (cli->options.interactive &&
        InteractiveWouldAsk(operands, cli->options)) {
      return ReportUnsupported(argv0, 'i');
    }
    if (cli->options.prompt_once &&
        PromptOnceWouldAsk(operands, cli->options.recursive)) {
      return ReportUnsupported(argv0, 'I');
    }
    if (cli->options.one_file_system && cli->options.recursive &&
        OneFileSystemWouldMatter(operands)) {
      return ReportUnsupported(argv0, 'x');
    }
  }

  const bool force = cli->options.force;
  const std::uint16_t threads = ThreadCount();
  FileUnlinkWorker prototype;
  prototype.force_ = force;
  Scheduler scheduler(threads, prototype);

  std::size_t failures = guarded.refused ? 1 : 0;
  for (const char* path : operands) {
    struct stat path_stat;
    if (cutils::os::lstat(path, &path_stat) != 0) {
      if (const int error = errno; StatFailureReportable(cli->options, error)) {
        ReportError(path, error);
        ++failures;
      }
      continue;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
      if (cutils::os::unlink(path) != 0) {
        if (const int error = errno; Reportable(force, error)) {
          ReportError(path, error);
          ++failures;
        }
      }
      continue;
    }

    if (!cli->options.recursive) {
      if (cli->options.dir) {
        // -d: rmdir(2) the operand as given, so "l/" removes the link's
        // target and "//" fails with EISDIR, and report errno like unlink.
        if (cutils::os::rmdir(path) != 0) {
          if (const int error = errno; Reportable(force, error)) {
            ReportError(path, error);
            ++failures;
          }
        }
        continue;
      }
      // rm's own text, not strerror(EISDIR)'s "Is a directory".
      WriteStderr({ProgramName(), ": ", path, ": is a directory\n"});
      ++failures;
      continue;
    }

    auto dirfd =
        cutils::os::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dirfd) {
      // As in the walk: under -f, rm removes an unreadable empty directory.
      if (!force || !DirGone(cutils::os::rmdir(path))) {
        ReportError(path, static_cast<int>(dirfd.error().code));
        ++failures;
      }
      continue;
    }

    // Seeding precedes Run, so this is still single-threaded.
    SeedRoot(scheduler, *std::move(dirfd), path);
  }

  (void)scheduler.Wait();

  for (const auto& worker : scheduler.Workers()) {
    failures += worker.failures_;
  }

  return failures == 0 ? 0 : 1;
}
