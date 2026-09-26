// See LICENSE in the repository root.

/// Remmy is `rm` that's a lot faster on macOS.
///
/// It's pretty much as fast I could make `rm` go. If you manage to make a
/// faster `rm`, **please** contact me. I would love to see it =)
///
/// The general algorithm is here described.
///
/// The program starts off in single-threaded mode. It takes the positional
/// paths given in the input CLI one at a time, in order, and stats each: if it
/// is a file, it deletes it. If the path is a directory, however, this program
/// opens the directory and emplaces the new open FD in a multi-threaded
/// scheduler. As rm does, it then waits for the whole tree to be removed
/// before it looks at the next path, unless that cannot make a difference
/// (sibling directories, as in `rm -rf dir/*`, are walked together).
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
#include <cutils/io/stdout_writer.hpp>
#include <cutils/io/warn.hpp>
#include <cutils/io/writer_ref.hpp>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/variant.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
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

/// @brief Puts each operand's diagnostics on stderr only once those of the
///        operands before it are out, so that stderr reads as if the operands
///        were removed one after another, as rm removes them, while walks of
///        several operands run at once (see ConcurrentWalks).
///
/// The first operand not yet finished writes straight through; the others'
/// diagnostics are held until it is their turn. Only failures and operand
/// ends come here, so the lock is off the removal path.
class OrderedStderr {
 public:
  explicit OrderedStderr(std::size_t operands)
      : held_(operands), finished_(operands, false) {}

  /// @brief Writes the pieces, one diagnostic of `operand`, or holds them
  ///        back while an earlier operand is not finished.
  template <cutils::io::PieceRange R>
  auto Write(std::size_t operand, R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    const std::lock_guard lock(mutex_);
    if (operand == next_) {
      return cutils::io::stderr_writer.WriteMany(std::forward<R>(pieces));
    }
    for (const std::string_view piece : pieces) {
      held_[operand].append(piece);
    }
    return {};
  }

  /// @brief Marks `operand` as done with, which writes out what the operands
  ///        after it held back, up to the next one not done.
  auto Finish(std::size_t operand) noexcept -> void {
    const std::lock_guard lock(mutex_);
    finished_[operand] = true;
    while (next_ < finished_.size() && finished_[next_]) {
      ++next_;
      if (next_ < held_.size() && !held_[next_].empty()) {
        std::ignore = cutils::io::stderr_writer.Write(held_[next_]);
        std::string().swap(held_[next_]);
      }
    }
  }

 private:
  std::mutex mutex_;
  /// @brief The first operand not finished yet.
  std::size_t next_ = 0;
  std::vector<std::string> held_;
  std::vector<bool> finished_;
};

struct OperandStderr {
  OrderedStderr& ordered;
  std::size_t operand;

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <cutils::io::PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    return ordered.Write(operand, std::forward<R>(pieces));
  }

  [[nodiscard]] static auto Flush() noexcept -> std::expected<void, std::errc> {
    return {};
  }
};

/// @brief Report a failure the same way BSD rm's does.
auto WarnAt(OperandStderr to, std::string_view prog, std::string_view path,
            int error) noexcept -> void {
  std::ignore = cutils::io::Warn(to, prog, error, "{}", path);
}

/// @brief WarnAt for the entry `name` of the directory at `dir`.
auto WarnAt(OperandStderr to, std::string_view prog, std::string_view dir,
            std::string_view name, int error) noexcept -> void {
  std::ignore = cutils::io::Warn(to, prog, error, "{}/{}", dir, name);
}

template <class... Args>
auto LogIfRemoved(const remmy::Cli& cli, cutils::io::StdoutWriter& out,
                  int status,
                  cutils::io::FormatString<std::type_identity_t<Args>...> fmt,
                  const Args&... args) noexcept -> int {
  if (status == 0 && cli.Options().verbose) {
    std::ignore = cutils::io::PrintLn(out, fmt, args...);
  }
  return status;
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
  explicit FileUnlinkWorker(const remmy::Cli& cli,
                            cutils::io::StdoutWriter& removed,
                            OrderedStderr& ordered) noexcept
      : cli_(cli), stdout_(removed), ordered_(ordered) {}

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
          WarnAt(To(task), cli_.CommandName(), task->PathInto(path_buffer_),
                 static_cast<int>(err.code));
        }

        DirNode* parent = task->parent_;
        const std::uint32_t operand = task->operand_;
        delete task;
        if (parent != nullptr) {
          MaybeCleanupDirNode(parent);
        } else {
          ordered_.Finish(operand);
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
        cli_.Options().verbose ? task->PathInto(scan_path_) : "";
    for (const auto& read : dirs_.Read(task->fd_)) {
      if (!read) {
        // A failed read is not the end of the directory; say so and stop.
        failures_++;
        WarnAt(To(task), cli_.CommandName(), task->PathInto(path_buffer_),
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
            WarnAt(To(task), cli_.CommandName(), task->PathInto(path_buffer_),
                   entry.name(), errno);
          }
          continue;
        }
        is_dir = S_ISDIR(st.st_mode);
      }

      if (!is_dir) {
        if (LogIfRemoved(cli_, stdout_,
                         cutils::os::unlinkat(task->fd_, entry.c_str(), 0),
                         "{}/{}", dir_path, entry.name()) != 0 &&
            errno != ENOENT) {
          failures_++;
          WarnAt(To(task), cli_.CommandName(), task->PathInto(path_buffer_),
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
              (LogIfRemoved(
                   cli_, stdout_,
                   cutils::os::unlinkat(task->fd_, entry.c_str(), AT_REMOVEDIR),
                   "{}/{}", dir_path, entry.name()) != 0 &&
               errno != ENOENT)) {
            failures_++;
            WarnAt(To(task), cli_.CommandName(), task->PathInto(path_buffer_),
                   entry.name(), static_cast<int>(opened.error().code));
          }
          continue;
        }
      }

      // Count the child on the parent before publishing it: a worker could
      // steal and finish it the instant Submit returns, and the parent's own
      // scan reference (held until Finish, below) keeps
      // `remaining_children_dirs_` from reaching zero mid-scan regardless.
      auto* child = new DirNode(std::move(child_fd), task,
                                std::string{entry.name()}, task->operand_);
      task->remaining_children_dirs_.fetch_add(1, std::memory_order_relaxed);
      const std::size_t tier =
          child->fd_.IsOpen() ? kRunnable : kAwaitingDescriptor;

      // The scheduler carries raw pointers (its slots are trivially copyable
      // and a steal may briefly duplicate one), so ownership is manual: the
      // node is freed by whichever worker drops its last reference in Finish.
      ctx.Submit(child, tier);
    }
  }

  auto RemoveEmptyLogged(const DirNode* node) noexcept -> int {
    return LogIfRemoved(cli_, stdout_, node->RemoveEmpty(path_buffer_), "{}",
                        path_buffer_);
  }

  /// @brief Where diagnostics about `node` go.
  [[nodiscard]] auto To(const DirNode* node) const noexcept -> OperandStderr {
    return {.ordered = ordered_, .operand = node->operand_};
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
        WarnAt(To(current), cli_.CommandName(), path_buffer_, errno);
      }

      const bool root = current->parent_ == nullptr;
      const std::uint32_t operand = current->operand_;
      delete current;
      if (root) {
        // The operand's walk is over.
        ordered_.Finish(operand);
      }
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
  cutils::io::StdoutWriter& stdout_;

  /// @brief Where diagnostics go, in operand order; told when an operand's
  ///        walk is over.
  OrderedStderr& ordered_;

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
    std::ignore =
        cutils::io::Warn(cutils::io::stderr_writer, prog, errno, "{}", path);
    return 1;
  }

  if (S_ISDIR(path_stat.st_mode)) {
    std::ignore = cutils::io::Warnx(cutils::io::stderr_writer, prog,
                                    "{}: is a directory", path);
    return 1;
  }

  if (cutils::os::unlink(path) != 0) {
    std::ignore =
        cutils::io::Warn(cutils::io::stderr_writer, prog, errno, "{}", path);
    return 1;
  }

  return 0;
}

/// @brief Starts the walk of operand number `operand`, the directory `path`
///        open on `dirfd`; false when it could not be started.
auto SeedRoot(Scheduler& scheduler, cutils::os::Fd dirfd, std::string_view path,
              std::size_t operand) -> bool {
  auto* task = new DirNode(std::move(dirfd), nullptr, std::string{path},
                           static_cast<std::uint32_t>(operand));
  if (!scheduler.Submit(task)) {
    delete task;
    return false;
  }
  return true;
}

/// @brief The directory an operand names its entry in, as typed, when it
///        takes no more than that: "" for a bare name ("a"), `P` for "P/a"
///        where P has no empty, "." or ".." component, and "/" for "/a".
///
/// Any other shape has none: a trailing slash (which follows a symlink), or a
/// path that climbs or repeats a slash.
constexpr auto ParentAsTyped(std::string_view path) noexcept
    -> std::optional<std::string_view> {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return path.empty() ? std::nullopt
                        : std::optional<std::string_view>(std::string_view());
  }
  const std::string_view name = path.substr(slash + 1);
  if (name.empty() || name == "." || name == "..") {
    return std::nullopt;
  }
  if (slash == 0) {
    return path.substr(0, 1);
  }
  const std::string_view parent = path.substr(0, slash);
  std::string_view rest = parent.front() == '/' ? parent.substr(1) : parent;
  while (true) {
    const std::size_t next = rest.find('/');
    const std::string_view component = rest.substr(0, next);
    if (component.empty() || component == "." || component == "..") {
      return std::nullopt;
    }
    if (next == std::string_view::npos) {
      return parent;
    }
    rest.remove_prefix(next + 1);
  }
}

/// @brief Whether resolving `parent` (see ParentAsTyped) only goes down into
///        real directories: none of its leading paths is a symlink.
auto ResolvesDownward(std::string_view parent) -> bool {
  if (parent.empty() || parent == "/") {
    return true;
  }
  for (std::size_t end = parent.find('/', 1);;
       end = parent.find('/', end + 1)) {
    const std::string leading(parent.substr(0, end));
    struct stat leading_stat;
    if (cutils::os::lstat(leading.c_str(), &leading_stat) != 0 ||
        !S_ISDIR(leading_stat.st_mode)) {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
  }
}

/// @brief The walks main leaves running while it goes on with later operands.
///
/// rm is done with an operand, its whole walk included, before it looks at the
/// next one, and remmy removes them in that order too: a later operand may be
/// the same directory, lie inside it, contain it or name a path through it.
/// Only where none of that can happen does main go on while a walk runs, so
/// that many directory operands (`rm -rf dir/*`) are still walked together:
/// when the walked operands and the next one are entries of one directory,
/// named through the same path (see ParentAsTyped) of real directories only
/// (see ResolvesDownward), removing one of them cannot change what another's
/// path leads to. A next operand that is one of the walked directories under
/// another name ("d" and "D" on a case-insensitive volume) still waits.
/// OrderedStderr keeps the diagnostics in operand order meanwhile.
class ConcurrentWalks {
 public:
  /// @brief Whether an operand named in `parent` may be looked at while the
  ///        walks run.
  [[nodiscard]] auto Admits(
      std::optional<std::string_view> parent) const noexcept -> bool {
    return roots_.empty() || (parent.has_value() && *parent == parent_);
  }

  /// @brief Whether the directory `inode` is the root of a running walk.
  [[nodiscard]] auto Walks(ino_t inode) const noexcept -> bool {
    return roots_.contains(inode);
  }

  /// @brief Records the walk just started of the directory `inode`, named in
  ///        `parent`; false when main has to wait for it instead.
  auto Add(std::optional<std::string_view> parent, ino_t inode) -> bool {
    if (!parent.has_value()) {
      return false;
    }
    if (roots_.empty()) {
      if (!ResolvesDownward(*parent)) {
        return false;
      }
      parent_ = *parent;
    }
    roots_.insert(inode);
    return true;
  }

  /// @brief Forgets the walks, once they are over.
  auto Clear() noexcept -> void { roots_.clear(); }

 private:
  std::string_view parent_;
  /// @brief The walked directories, by inode alone: a match on another
  ///        device only costs a wait.
  std::unordered_set<ino_t> roots_;
};

auto RunRm(const remmy::Cli& cli) -> int {
  if (cli.Options().prompt_once &&
      !ConfirmPromptOnce(cli.Operands(), cli.Options().recursive)) {
    return 1;
  }

  const bool force = cli.Options().force;
  cutils::io::StdoutWriter stdout(
      {.shared = static_cast<std::size_t>(
           cli.Options().verbose && ::isatty(STDOUT_FILENO) == 0 ? 64 * 1024
                                                                 : 0),
       .handle = 0},
      STDOUT_FILENO);

  const std::span<char* const> operands = cli.Operands();
  // Declared before the scheduler, so it outlives the workers holding it.
  OrderedStderr ordered(operands.size());

  const std::uint16_t threads = ThreadCount();
  const std::string_view prog = cli.CommandName();
  const FileUnlinkWorker prototype(cli, stdout, ordered);
  Scheduler scheduler(threads, prototype);

  ConcurrentWalks walks;
  const auto finish_walks = [&scheduler, &walks] {
    scheduler.Drain();
    walks.Clear();
  };

  std::size_t failures = cli.HasDroppedOperands() ? 1 : 0;
  for (std::size_t operand = 0; operand < operands.size(); ++operand) {
    const char* path = operands[operand];
    OperandStderr report{.ordered = ordered, .operand = operand};
    const std::optional<std::string_view> parent = ParentAsTyped(path);
    if (!walks.Admits(parent)) {
      finish_walks();
    }

    // Removes the operand, or opens it when it is a directory to walk.
    struct stat path_stat;
    const auto remove_or_open = [&]() -> cutils::os::Fd {
      int status = cutils::os::lstat(path, &path_stat);
      if (status == 0 && S_ISDIR(path_stat.st_mode) &&
          walks.Walks(path_stat.st_ino)) {
        // A directory being walked, named again: as rm would, finish that
        // walk first and look again.
        finish_walks();
        status = cutils::os::lstat(path, &path_stat);
      }
      if (status != 0) {
        if (const int error = errno;
            !(force && cli.Options().recursive && cutils::os::GetEUid() != 0) &&
            (!force || error != ENOENT)) {
          WarnAt(report, prog, path, error);
          ++failures;
        }
        return {};
      }

      if (!S_ISDIR(path_stat.st_mode)) {
        if (LogIfRemoved(cli, stdout, cutils::os::unlink(path), "{}", path) !=
            0) {
          if (const int error = errno; !force || error != ENOENT) {
            WarnAt(report, prog, path, error);
            ++failures;
          }
        }
        return {};
      }

      if (!cli.Options().recursive) {
        if (cli.Options().dir) {
          if (LogIfRemoved(cli, stdout, cutils::os::rmdir(path), "{}", path) !=
                  0 &&
              (!force || errno != ENOENT)) {
            WarnAt(report, prog, path, errno);
            ++failures;
          }
          return {};
        }

        std::ignore =
            cutils::io::Warnx(report, prog, "{}: is a directory", path);
        ++failures;
        return {};
      }

      auto dirfd = cutils::os::open(
          path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!dirfd) {
        if (!force || (LogIfRemoved(cli, stdout, cutils::os::rmdir(path), "{}",
                                    path) != 0 &&
                       errno != ENOENT)) {
          WarnAt(report, prog, path, static_cast<int>(dirfd.error().code));
          ++failures;
        }
        return {};
      }
      return *std::move(dirfd);
    };

    cutils::os::Fd dirfd = remove_or_open();
    if (!dirfd.IsOpen() ||
        !SeedRoot(scheduler, std::move(dirfd), path, operand)) {
      ordered.Finish(operand);
      continue;
    }
    // The walk runs on every worker; unless it cannot matter (see
    // ConcurrentWalks), it is over before the next operand is looked at.
    // -v's lines follow operand order only that way.
    if (cli.Options().verbose || !walks.Add(parent, path_stat.st_ino)) {
      finish_walks();
    }
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
