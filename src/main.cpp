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
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
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
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
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

/// @brief -v's output: every removed path on a line of its own on stdout.
///
/// Buffered byte for byte like rm's stdio stdout, so that its lines land at
/// the same offsets among rm's (unbuffered) diagnostics when both go to one
/// file or pipe: the buffer is stdout's st_blksize (BUFSIZ when fstat(2) gives
/// none), line buffered on a terminal. Like __sfvwrite, a piece that no longer
/// fits tops the buffer up and writes it out whole, even mid-line, a piece at
/// least a buffer long goes out directly a buffer at a time, and a full buffer
/// waits for the next byte (or exit) to be written.
///
/// The workers share it under a lock. A path is added after its removal
/// succeeded, and a directory is only removed once its contents are, so a
/// directory's line follows those of its contents, and an operand's lines
/// follow those of the operands before it. The order among entries of one
/// directory and sibling subtrees, which remmy removes in parallel, can differ
/// from rm's.
///
/// Whatever is left is written out on destruction. Write failures are ignored,
/// as rm ignores them. Without -v there is no log: the workers hold a null
/// pointer, and the removal path pays one branch for it.
class RemovedLog {
 public:
  RemovedLog() noexcept {
    const int saved_errno = errno;
    struct stat out_stat;
    if (::fstat(STDOUT_FILENO, &out_stat) == 0) {
      if (out_stat.st_blksize > 0) {
        capacity_ = static_cast<std::size_t>(out_stat.st_blksize);
      }
      line_buffered_ =
          S_ISCHR(out_stat.st_mode) && ::isatty(STDOUT_FILENO) != 0;
    }
    buffer_.reserve(capacity_);
    errno = saved_errno;
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
        PutLocked(piece);
      }
      PutLocked("\n");
    }
    errno = saved_errno;
  }

  /// @brief Writes out what is buffered.
  auto Flush() noexcept -> void {
    const std::lock_guard lock(mutex_);
    FlushLocked();
  }

 private:
  /// @brief BUFSIZ on macOS, stdio's size when st_blksize is not positive.
  static constexpr std::size_t kFallbackCapacity = 1024;

  /// @brief __sfvwrite(3)'s placement of `bytes`; on a terminal each newline
  ///        also writes out the buffer.
  auto PutLocked(std::string_view bytes) noexcept -> void {
    while (!bytes.empty()) {
      std::size_t chunk = bytes.size();
      bool ends_line = false;
      if (line_buffered_) {
        if (const std::size_t newline = bytes.find('\n');
            newline != std::string_view::npos) {
          chunk = newline + 1;
          ends_line = true;
        }
      }
      PutChunkLocked(bytes.substr(0, chunk));
      bytes.remove_prefix(chunk);
      if (ends_line) {
        FlushLocked();
      }
    }
  }

  auto PutChunkLocked(std::string_view chunk) noexcept -> void {
    while (!chunk.empty()) {
      const std::size_t room = capacity_ - buffer_.size();
      if (!buffer_.empty() && chunk.size() > room) {
        // Fill and flush.
        buffer_.append(chunk.substr(0, room));
        chunk.remove_prefix(room);
        FlushLocked();
      } else if (chunk.size() >= capacity_) {
        // Write one buffer's worth directly.
        WriteAll(chunk.substr(0, capacity_));
        chunk.remove_prefix(capacity_);
      } else {
        // Fill and done.
        buffer_.append(chunk);
        chunk = {};
      }
    }
  }

  auto FlushLocked() noexcept -> void {
    WriteAll(buffer_);
    buffer_.clear();
  }

  static auto WriteAll(std::string_view rest) noexcept -> void {
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
  }

  std::mutex mutex_;
  std::string buffer_;
  std::size_t capacity_ = kFallbackCapacity;
  bool line_buffered_ = false;
};

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

/// @brief A path as rm's diagnostics print it: escaped C style, like vis(3)
///        with VIS_CSTYLE | VIS_NOSLASH, but only for C0 control bytes.
///
/// \a \b \v \f and \r get their C names and every other control byte its
/// three octal digits (ESC is \033), whatever the locale. TAB and LF stay raw,
/// as do the backslash, DEL, bytes of 0x80 and above and so every multibyte
/// character. Only diagnostics escape: -v's lines and prompts print paths raw.
///
/// A path with nothing to escape, the usual case, is viewed and not copied.
class EscapedPath {
 public:
  explicit EscapedPath(std::string_view path) noexcept : view_(path) {
    if (std::ranges::none_of(path, NeedsEscape)) {
      return;
    }
    static constexpr std::size_t kMaxEscapeLength = 4;
    escaped_.reserve(path.size() * kMaxEscapeLength);
    for (const char byte : path) {
      Append(byte);
    }
    view_ = escaped_;
  }

  EscapedPath(const EscapedPath&) = delete;
  auto operator=(const EscapedPath&) -> EscapedPath& = delete;
  EscapedPath(EscapedPath&&) = delete;
  auto operator=(EscapedPath&&) -> EscapedPath& = delete;
  ~EscapedPath() = default;

  [[nodiscard]] auto View() const noexcept -> std::string_view { return view_; }

 private:
  static constexpr auto NeedsEscape(char byte) noexcept -> bool {
    const auto value = static_cast<unsigned char>(byte);
    return value < static_cast<unsigned char>(' ') && byte != '\t' &&
           byte != '\n';
  }

  auto Append(char byte) noexcept -> void {
    if (!NeedsEscape(byte)) {
      escaped_.push_back(byte);
      return;
    }
    escaped_.push_back('\\');
    switch (byte) {
      case '\a':
        escaped_.push_back('a');
        return;
      case '\b':
        escaped_.push_back('b');
        return;
      case '\v':
        escaped_.push_back('v');
        return;
      case '\f':
        escaped_.push_back('f');
        return;
      case '\r':
        escaped_.push_back('r');
        return;
      default:
        break;
    }
    // A control byte is below 040, so its first octal digit is 0.
    static constexpr unsigned kOctalBits = 3;
    static constexpr unsigned kOctalDigitMask = 07;
    const auto value = static_cast<unsigned char>(byte);
    escaped_.push_back('0');
    escaped_.push_back(static_cast<char>('0' + (value >> kOctalBits)));
    escaped_.push_back(static_cast<char>('0' + (value & kOctalDigitMask)));
  }

  std::string_view view_;
  std::string escaped_;
};

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
  auto Write(std::size_t operand,
             std::initializer_list<std::string_view> pieces) noexcept -> void {
    const std::lock_guard lock(mutex_);
    if (operand == next_) {
      WriteStderr(pieces);
      return;
    }
    for (const std::string_view piece : pieces) {
      held_[operand].append(piece);
    }
  }

  /// @brief Marks `operand` as done with, which writes out what the operands
  ///        after it held back, up to the next one not done.
  auto Finish(std::size_t operand) noexcept -> void {
    const std::lock_guard lock(mutex_);
    finished_[operand] = true;
    while (next_ < finished_.size() && finished_[next_]) {
      ++next_;
      if (next_ < held_.size() && !held_[next_].empty()) {
        WriteStderr({held_[next_]});
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

/// @brief Where an operand's diagnostics go: in operand order through an
///        OrderedStderr, or, without one, straight to stderr.
struct ErrorSink {
  OrderedStderr* ordered = nullptr;
  std::size_t operand = 0;

  auto operator()(std::initializer_list<std::string_view> pieces) const noexcept
      -> void {
    if (ordered != nullptr) {
      ordered->Write(operand, pieces);
    } else {
      WriteStderr(pieces);
    }
  }
};

/// @brief Reports a failure the way BSD rm's warn(3) does:
///        "<prog>: <path>: <strerror>", with `path` escaped (see EscapedPath).
auto ReportError(const ErrorSink& to, std::string_view path, int error) noexcept
    -> void {
  const ErrorText text(error);
  const EscapedPath shown(path);
  to({ProgramName(), ": ", shown.View(), ": ", text.View(), "\n"});
}

/// @brief ReportError for the entry `name` of the directory at `dir`, which is
///        the path fts(3) gives rm for it.
auto ReportError(const ErrorSink& to, std::string_view dir,
                 std::string_view name, int error) noexcept -> void {
  const ErrorText text(error);
  const std::array<EscapedPath, 2> shown{EscapedPath(dir), EscapedPath(name)};
  to({ProgramName(), ": ", shown[0].View(), "/", shown[1].View(), ": ",
      text.View(), "\n"});
}

/// @brief Refuses a directory operand removed without -r, -R or -d, in rm's
///        own words, not strerror(EISDIR)'s "Is a directory".
auto ReportIsDirectory(const ErrorSink& to, std::string_view path) noexcept
    -> void {
  const EscapedPath shown(path);
  to({ProgramName(), ": ", shown.View(), ": is a directory\n"});
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

  /// @brief -v: where removed paths go; null without -v.
  RemovedLog* removed_ = nullptr;

  /// @brief Where diagnostics go, in operand order; told when an operand's
  ///        walk is over.
  OrderedStderr* ordered_ = nullptr;

  /// @brief Thread-local buffer holding the path of the directory being
  ///        scanned, for -v's lines about its entries.
  std::string scan_path_;

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
        if (!force_ || !DirGone(LogRemoval(cutils::os::rmdir(path), path))) {
          failures_++;
          ReportError(To(task), path, static_cast<int>(err.code));
        }
        DirNode* parent = task->parent_;
        const std::uint32_t operand = task->operand_;
        delete task;
        if (parent != nullptr) {
          MaybeCleanupDirNode(parent);
        } else {
          ordered_->Finish(operand);
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
        ReportError(To(task), task->PathInto(path_buffer_),
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
            ReportError(To(task), task->PathInto(path_buffer_), entry.name(),
                        errno);
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
          ReportError(To(task), task->PathInto(path_buffer_), entry.name(),
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
          if (!force_ ||
              !DirGone(LogRemoval(
                  cutils::os::unlinkat(task->fd_, entry.c_str(), AT_REMOVEDIR),
                  dir_path, entry.name()))) {
            failures_++;
            ReportError(To(task), task->PathInto(path_buffer_), entry.name(),
                        static_cast<int>(opened.error().code));
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

  /// @brief Where diagnostics about `node` go.
  [[nodiscard]] auto To(const DirNode* node) const noexcept -> ErrorSink {
    return {.ordered = ordered_, .operand = node->operand_};
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
      if (LogRemoval(cutils::os::rmdir(path), path) != 0 && errno != ENOENT) {
        failures_++;
        ReportError(To(current), path, errno);
      }

      const bool root = current->parent_ == nullptr;
      const std::uint32_t operand = current->operand_;
      delete current;
      if (root) {
        // The operand's walk is over.
        ordered_->Finish(operand);
      }
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

/// @brief The one operand of an unlink(1) command line, or nullptr when it is
///        a usage error.
///
/// unlink(1) parses no options: it takes exactly one operand, which a single
/// leading "--" may precede. So "-f" alone is a file's name, "--" alone is one
/// too, and "-f file" or "a b" are usage errors.
constexpr auto UnlinkOperand(std::span<char* const> args) noexcept
    -> const char* {
  if (args.size() == 1) {
    return args.front();
  }
  if (args.size() == 2 && std::string_view(args.front()) == "--") {
    return args.back();
  }
  return nullptr;
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

/// @brief BSD rm run as unlink(1) (see IsUnlinkMode); returns the exit status.
///
/// The operand goes straight to unlink(2), as rm's rm_file() hands it on
/// without -d, -f, -i or -v: no dot or slash guard, no prompt, and a directory
/// ("." ".." and "/" included) is refused with rm's "is a directory".
auto RunUnlink(std::string_view argv0, std::span<char* const> args) noexcept
    -> int {
  const char* path = UnlinkOperand(args);
  if (path == nullptr) {
    return ReportUsage(argv0, remmy::UsageError{});
  }

  const ErrorSink to_stderr;
  struct stat path_stat;
  if (cutils::os::lstat(path, &path_stat) != 0) {
    ReportError(to_stderr, path, errno);
    return 1;
  }
  if (S_ISDIR(path_stat.st_mode)) {
    ReportIsDirectory(to_stderr, path);
    return 1;
  }
  if (cutils::os::unlink(path) != 0) {
    ReportError(to_stderr, path, errno);
    return 1;
  }
  return 0;
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

/// @brief An operand read as an entry of a directory.
struct EntryOperand {
  /// @brief The directory, as typed: "" for the working directory ("a"), "/"
  ///        for the root ("/a"), `P` for "P/a".
  std::string_view parent;
  /// @brief The operand without its trailing slashes.
  std::string_view entry;
  /// @brief Whether the operand ended in slashes, which follow a symlink.
  bool trailing_slash;
};

/// @brief `path` read as an entry of a directory, or none when its last
///        component is "." or ".." or it has none at all ("/").
constexpr auto AsEntry(std::string_view path) noexcept
    -> std::optional<EntryOperand> {
  std::string_view entry = path;
  while (entry.size() > 1 && entry.back() == '/') {
    entry.remove_suffix(1);
  }
  const std::size_t slash = entry.rfind('/');
  const std::string_view name =
      slash == std::string_view::npos ? entry : entry.substr(slash + 1);
  if (name.empty() || name == "." || name == "..") {
    return std::nullopt;
  }
  std::string_view parent;
  if (slash != std::string_view::npos) {
    parent = entry.substr(0, slash + 1);
    while (parent.size() > 1 && parent.back() == '/') {
      parent.remove_suffix(1);
    }
  }
  return EntryOperand{.parent = parent,
                      .entry = entry,
                      .trailing_slash = entry.size() != path.size()};
}

/// @brief Whether `path` is a directory itself, not a symlink to one.
auto IsRealDirectory(std::string_view path) -> bool {
  const std::string terminated(path);
  struct stat path_stat;
  return cutils::os::lstat(terminated.c_str(), &path_stat) == 0 &&
         S_ISDIR(path_stat.st_mode);
}

/// @brief A file, by identity.
struct FileId {
  dev_t device;
  ino_t inode;

  auto operator==(const FileId&) const -> bool = default;
};

/// @brief The identity of the directory open on `dir`.
auto IdOf(const cutils::os::Fd& dir) noexcept -> std::optional<FileId> {
  struct stat dir_stat;
  if (cutils::os::fstatat(dir, ".", &dir_stat, 0) != 0) {
    return std::nullopt;
  }
  return FileId{.device = dir_stat.st_dev, .inode = dir_stat.st_ino};
}

/// @brief The directory `parent` (see EntryOperand) resolves to, provided that
///        removing the trees under that directory's entries cannot change
///        what it resolves to.
///
/// It follows `parent` the way the kernel resolves it, component by component,
/// symlinks, "." and ".." included, and keeps every directory it looks a name
/// up in. It then rejects `parent` if one of those lies below the directory
/// it resolved to: inside one of its entries, where a walk of that entry would
/// remove it. Directories above it, which `/tmp` -> `private/tmp` passes
/// through, are out of reach of those walks. Anything it cannot follow (an
/// unreadable directory, a symlink loop) is rejected too.
///
/// The symlinks it follows are added to `links`: removing one of them would
/// change what `parent` resolves to.
auto ResolvesAbove(std::string_view parent, std::vector<FileId>& links)
    -> std::optional<FileId> {
  constexpr int kOpenDir = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
  // MAXSYMLINKS: where the kernel gives up with ELOOP.
  constexpr int kMaxLinks = 32;
  // Past this depth, a directory tree is not worth reasoning about.
  constexpr int kMaxDepth = 1024;

  std::vector<cutils::os::Fd> visited;
  const auto enter_root = [&visited](bool absolute) -> bool {
    auto dir = cutils::os::open(absolute ? "/" : ".", kOpenDir);
    if (!dir) {
      return false;
    }
    visited.push_back(*std::move(dir));
    return true;
  };

  if (!enter_root(!parent.empty() && parent.front() == '/')) {
    return std::nullopt;
  }
  std::string rest(parent);
  std::size_t at = 0;
  int followed = 0;
  while (true) {
    while (at < rest.size() && rest[at] == '/') {
      ++at;
    }
    if (at == rest.size()) {
      break;
    }
    const std::size_t end = std::min(rest.find('/', at), rest.size());
    const std::string name = rest.substr(at, end - at);
    at = end;
    if (name == ".") {
      continue;
    }
    const bool up = name == "..";
    auto next = cutils::os::openat(visited.back(), name.c_str(),
                                   up ? kOpenDir : kOpenDir | O_NOFOLLOW);
    if (next) {
      visited.push_back(*std::move(next));
      continue;
    }
    // Not a directory to go into: follow it if it is a symlink.
    std::array<char, PATH_MAX> target{};
    const ssize_t length = ::readlinkat(visited.back().get(), name.c_str(),
                                        target.data(), target.size());
    struct stat link_stat;
    if (up || length <= 0 || std::cmp_greater_equal(length, target.size()) ||
        ++followed > kMaxLinks ||
        cutils::os::fstatat(visited.back(), name.c_str(), &link_stat,
                            AT_SYMLINK_NOFOLLOW) != 0) {
      return std::nullopt;
    }
    links.push_back({.device = link_stat.st_dev, .inode = link_stat.st_ino});
    const std::string_view link(target.data(),
                                static_cast<std::size_t>(length));
    rest = std::string(link) + "/" + rest.substr(at);
    at = 0;
    if (link.front() == '/' && !enter_root(true)) {
      return std::nullopt;
    }
  }

  const std::optional<FileId> resolved = IdOf(visited.back());
  if (!resolved) {
    return std::nullopt;
  }
  // Directories known to be out of reach: from `resolved` up to the root, and
  // then those above the other visited directories.
  std::vector<FileId> above;
  // Whether `start` is out of reach: it climbs to the root, or to a directory
  // already known out of reach, without passing `resolved`.
  const auto out_of_reach = [&above, &resolved](const cutils::os::Fd& start) {
    std::optional<FileId> id = IdOf(start);
    cutils::os::Fd dir;
    for (int depth = 0; id.has_value() && depth < kMaxDepth; ++depth) {
      if (depth > 0 && *id == *resolved) {
        return false;
      }
      if (std::ranges::contains(above, *id)) {
        return true;
      }
      above.push_back(*id);
      auto parent_dir =
          cutils::os::openat(depth == 0 ? start : dir, "..", kOpenDir);
      if (!parent_dir) {
        return false;
      }
      const std::optional<FileId> parent_id = IdOf(*parent_dir);
      if (parent_id == id) {
        // The root is its own parent.
        return true;
      }
      dir = *std::move(parent_dir);
      id = parent_id;
    }
    return false;
  };
  if (!out_of_reach(visited.back())) {
    return std::nullopt;
  }
  for (const cutils::os::Fd& dir : visited) {
    if (!out_of_reach(dir)) {
      return std::nullopt;
    }
  }
  return resolved;
}

/// @brief The walks main leaves running while it goes on with later operands.
///
/// rm is done with an operand, its whole walk included, before it looks at the
/// next one, and remmy removes them in that order too: a later operand may be
/// the same directory, lie inside it, contain it or name a path through it.
/// Only where none of that can happen does main go on while a walk runs, so
/// that many directory operands (`rm -rf dir/*`, `rm -rf */`) are still walked
/// together: when the walked operands and the next one are entries of one
/// directory, named through paths that resolve to it without passing below it
/// (see ResolvesAbove), removing one of them cannot change what another's path
/// leads to. A next operand that is one of the walked directories under
/// another name ("d" and "D" on a case-insensitive volume) or a symlink on the
/// way to them still waits, and so does one with a trailing slash that is not
/// a directory itself (a symlink, which the slash follows). OrderedStderr keeps
/// the diagnostics in operand order meanwhile.
class ConcurrentWalks {
 public:
  /// @brief Whether a walk is running.
  [[nodiscard]] auto Running() const noexcept -> bool {
    return !roots_.empty();
  }

  /// @brief Whether an operand, read as `entry`, may be looked at while the
  ///        walks run.
  [[nodiscard]] auto Admits(const std::optional<EntryOperand>& entry) -> bool {
    if (roots_.empty()) {
      return true;
    }
    if (!entry.has_value()) {
      return false;
    }
    if (entry->parent == parent_) {
      return true;
    }
    if (ResolvesAbove(entry->parent, links_) != directory_) {
      return false;
    }
    parent_.assign(entry->parent);
    return true;
  }

  /// @brief Whether the running walks depend on the file `path_stat` is
  ///        about: it is the root of one of them, or a symlink the path to
  ///        their roots goes through.
  [[nodiscard]] auto Involve(const struct stat& path_stat) const noexcept
      -> bool {
    if (S_ISDIR(path_stat.st_mode)) {
      return roots_.contains(path_stat.st_ino);
    }
    return S_ISLNK(path_stat.st_mode) &&
           std::ranges::contains(links_, FileId{.device = path_stat.st_dev,
                                                .inode = path_stat.st_ino});
  }

  /// @brief Records the walk just started of the directory `inode`, read as
  ///        `entry` and admitted (see Admits); false when main has to wait for
  ///        it instead.
  auto Add(const std::optional<EntryOperand>& entry, ino_t inode) -> bool {
    if (!entry.has_value()) {
      return false;
    }
    if (roots_.empty()) {
      const std::optional<FileId> directory =
          ResolvesAbove(entry->parent, links_);
      if (!directory) {
        return false;
      }
      directory_ = *directory;
      parent_.assign(entry->parent);
    }
    roots_.insert(inode);
    return true;
  }

  /// @brief Forgets the walks, once they are over.
  auto Clear() noexcept -> void {
    roots_.clear();
    links_.clear();
  }

 private:
  /// @brief The directory the walked operands are entries of.
  FileId directory_{};
  /// @brief A name for it, as typed, known to resolve to it (see
  ///        ResolvesAbove).
  std::string parent_;
  /// @brief The walked directories, by inode alone: a match on another
  ///        device only costs a wait.
  std::unordered_set<ino_t> roots_;
  /// @brief The symlinks followed to reach the directory, by any of the names
  ///        admitted for it (see ResolvesAbove). Walks rebuild paths from
  ///        those names, so removing one has to wait for them.
  std::vector<FileId> links_;
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::span<char* const> args(
      argv, static_cast<std::size_t>(argc > 0 ? argc : 0));
  const char* argv0 = args.empty() ? "rm" : args.front();
  const auto rest = args.empty() ? args : args.subspan(1);
  if (IsUnlinkMode(argv0)) {
    return RunUnlink(argv0, rest);
  }
  const auto cli = remmy::ParseCli(rest);
  if (!cli) {
    return ReportUsage(argv0, cli.error());
  }
  const GuardedOperands guarded = ApplyGuards(cli->operands);
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
  // Declared before the scheduler, so it outlives the workers holding it and
  // flushes once they are done.
  std::optional<RemovedLog> removed;
  if (cli->options.verbose) {
    removed.emplace();
  }
  // -v: operands are logged exactly as typed, and walks start from them.
  const auto log_removed = [&removed](int status, const char* path) {
    if (status == 0 && removed) {
      removed->Add({path});
    }
    return status;
  };

  // Declared before the scheduler, so it outlives the workers holding it.
  OrderedStderr ordered(operands.size());

  const std::uint16_t threads = ThreadCount();
  FileUnlinkWorker prototype;
  prototype.force_ = force;
  prototype.removed_ = removed ? &*removed : nullptr;
  prototype.ordered_ = &ordered;
  Scheduler scheduler(threads, prototype);

  ConcurrentWalks walks;
  const auto finish_walks = [&scheduler, &walks] {
    scheduler.Drain();
    walks.Clear();
  };

  std::size_t failures = guarded.refused ? 1 : 0;
  for (std::size_t operand = 0; operand < operands.size(); ++operand) {
    const char* path = operands[operand];
    const ErrorSink report{.ordered = &ordered, .operand = operand};
    std::optional<EntryOperand> entry = AsEntry(path);
    if (!walks.Admits(entry)) {
      finish_walks();
    }
    if (entry.has_value() && entry->trailing_slash &&
        !IsRealDirectory(entry->entry)) {
      // "l/" follows the symlink `l` to wherever it leads, which need not be
      // an entry of the directory at all.
      entry.reset();
      if (walks.Running()) {
        finish_walks();
      }
    }

    // Removes the operand, or opens it when it is a directory to walk.
    struct stat path_stat;
    const auto remove_or_open = [&]() -> cutils::os::Fd {
      int status = cutils::os::lstat(path, &path_stat);
      if (status == 0 && walks.Involve(path_stat)) {
        // A directory being walked, named again, or a symlink on the way to
        // one: as rm would, finish the walks first and look again.
        finish_walks();
        status = cutils::os::lstat(path, &path_stat);
      }
      if (status != 0) {
        if (const int error = errno;
            StatFailureReportable(cli->options, error)) {
          ReportError(report, path, error);
          ++failures;
        }
        return {};
      }

      if (!S_ISDIR(path_stat.st_mode)) {
        if (log_removed(cutils::os::unlink(path), path) != 0) {
          if (const int error = errno; Reportable(force, error)) {
            ReportError(report, path, error);
            ++failures;
          }
        }
        return {};
      }

      if (!cli->options.recursive) {
        if (cli->options.dir) {
          // -d: rmdir(2) the operand as given, so "l/" removes the link's
          // target and "//" fails with EISDIR, and report errno like unlink.
          if (log_removed(cutils::os::rmdir(path), path) != 0) {
            if (const int error = errno; Reportable(force, error)) {
              ReportError(report, path, error);
              ++failures;
            }
          }
          return {};
        }
        ReportIsDirectory(report, path);
        ++failures;
        return {};
      }

      auto dirfd = cutils::os::open(
          path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!dirfd) {
        // As in the walk: under -f, rm removes an unreadable empty directory.
        if (!force || !DirGone(log_removed(cutils::os::rmdir(path), path))) {
          ReportError(report, path, static_cast<int>(dirfd.error().code));
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
    if (cli->options.verbose || !walks.Add(entry, path_stat.st_ino)) {
      finish_walks();
    }
  }

  scheduler.Wait();

  for (const auto& worker : scheduler.Workers()) {
    failures += worker.failures_;
  }

  return failures == 0 ? 0 : 1;
}
