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
#include <cutils/io/print.hpp>
#include <cutils/io/stderr_writer.hpp>
#include <cutils/io/stdout_writer.hpp>
#include <cutils/io/warn.hpp>
#include <cutils/os/attr_dir_reader.hpp>
#include <cutils/os/env.hpp>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <cutils/variant.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include "cli.hpp"
#include "dir_node.hpp"
#include "policy.hpp"
#include "prompt.hpp"
#include "prompter.hpp"
#include "remover.hpp"
#include "request.hpp"
#include "unlink_worker.hpp"

namespace {

using remmy::DirNode;

static auto ThreadCount() -> std::uint16_t {
  static constexpr const char* kEnvThreadsName = "REMMY_THREADS";
  static constexpr unsigned kMaxThreads = 4;

  const auto n = cutils::GetEnvOrDefault<std::uint16_t>(kEnvThreadsName, 0);
  if (n > 0) {
    return n;
  }

  return static_cast<std::uint16_t>(
      std::min(cutils::os::PerformanceCores(), kMaxThreads));
}

/// @brief Report a failure the same way BSD rm's does.
auto WarnAt(std::string_view prog, std::string_view path, int error) noexcept
    -> void {
  std::ignore =
      cutils::io::Warn(cutils::io::stderr_writer, prog, error, "{}", path);
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
    std::ignore = cutils::io::Warnx(cutils::io::stderr_writer, prog,
                                    "{}: is a directory", path);
    return 1;
  }

  if (cutils::os::unlink(path) != 0) {
    WarnAt(prog, path, errno);
    return 1;
  }

  return 0;
}

template <class Scheduler>
auto SeedRoot(Scheduler& scheduler, cutils::os::Fd dirfd, std::string_view path,
              bool needs_override) -> void {
  auto* task = new DirNode(std::move(dirfd), nullptr, std::string{path});
  task->needs_override_ = needs_override;
  if (!scheduler.Submit(task)) {
    delete task;
  }
}

/// @brief The gate a policy's workers write through.
template <class Policy>
auto GateFor(remmy::Console& console) noexcept -> typename Policy::Gate& {
  if constexpr (Policy::kPrompts) {
    return console.Gate();
  } else {
    static constinit typename Policy::Gate gate;
    return gate;
  }
}

/// @brief Whether rm's override prompt comes before removing the operand
///        `path`, whose lstat is `st`.
template <class Policy>
auto OperandNeedsOverride(const Policy& policy, const char* path,
                          const struct stat& st) noexcept -> bool {
  if constexpr (Policy::kReadsAttrs) {
    const auto kind = S_ISDIR(st.st_mode)   ? cutils::os::EntryKind::kDirectory
                      : S_ISLNK(st.st_mode) ? cutils::os::EntryKind::kSymlink
                                            : cutils::os::EntryKind::kOther;
    const bool writable =
        kind != cutils::os::EntryKind::kSymlink && ::access(path, W_OK) == 0;
    return policy.NeedsOverride(kind, writable,
                                remmy::OverrideInfo::From(st).flags);
  } else {
    return false;
  }
}

/// @brief The question about one operand before it is removed or entered;
///        yes when the policy asks none.
template <class Policy>
auto ConfirmOperand(remmy::Console& console, const Policy& policy,
                    const char* path, const struct stat& st,
                    bool enters) noexcept -> remmy::Answer {
  if constexpr (Policy::kAsksEveryEntry) {
    return enters && policy.AsksToExamine()
               ? console.Ask(remmy::ExaminePrompt{.path = path})
               : console.Ask(remmy::RemovePrompt{.path = path});
  } else {
    // A directory walked under -r is asked about at its rmdir, if at all.
    if (enters || !OperandNeedsOverride(policy, path, st)) {
      return remmy::Answer::kYes;
    }
    return console.Ask(remmy::OverridePrompt{
        .info = remmy::OverrideInfo::From(st), .path = path});
  }
}

template <class Policy>
auto RunRmWith(const remmy::Cli& cli, remmy::Console& console, Policy policy)
    -> int {
  using Worker = remmy::FileUnlinkWorker<Policy>;
  using Scheduler =
      cutils::TaskScheduler<Worker, Worker::kRanks, Policy::kIdle>;

  const bool force = cli.Options().force;
  cutils::io::StdoutWriter stdout(
      {.shared = static_cast<std::size_t>(
           cli.Options().verbose && ::isatty(STDOUT_FILENO) == 0 ? 64 * 1024
                                                                 : 0),
       .handle = 0},
      STDOUT_FILENO);

  const std::uint16_t threads = ThreadCount();
  const std::string_view prog = cli.CommandName();
  remmy::RequestList requests;
  auto& gate = GateFor<Policy>(console);
  const auto remover = [&] {
    return remmy::Remover<Policy>(
        cli, policy, typename remmy::Remover<Policy>::Output(gate, stdout),
        Policy::kPrompts ? &requests : nullptr);
  };
  const Worker prototype(remover());
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
      if (ConfirmOperand(console, policy, path, path_stat, false) ==
          remmy::Answer::kNo) {
        continue;
      }
      if (LogIfRemoved(cli, stdout, cutils::os::unlink(path), "{}", path) !=
          0) {
        if (const int error = errno; !force || error != ENOENT) {
          WarnAt(prog, path, error);
          ++failures;
        }
      }
      continue;
    }

    if (!cli.Options().recursive) {
      if (cli.Options().dir) {
        if (ConfirmOperand(console, policy, path, path_stat, false) ==
            remmy::Answer::kNo) {
          continue;
        }
        if (LogIfRemoved(cli, stdout, cutils::os::rmdir(path), "{}", path) !=
                0 &&
            (!force || errno != ENOENT)) {
          WarnAt(prog, path, errno);
          ++failures;
        }

        continue;
      }

      std::ignore = cutils::io::Warnx(cutils::io::stderr_writer, prog,
                                      "{}: is a directory", path);
      ++failures;
      continue;
    }

    if (ConfirmOperand(console, policy, path, path_stat, true) ==
        remmy::Answer::kNo) {
      continue;
    }

    auto dirfd =
        cutils::os::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dirfd) {
      if (!force || (LogIfRemoved(cli, stdout, cutils::os::rmdir(path), "{}",
                                  path) != 0 &&
                     errno != ENOENT)) {
        WarnAt(prog, path, static_cast<int>(dirfd.error().code));
        ++failures;
      }
      continue;
    }

    SeedRoot(scheduler, *std::move(dirfd), path,
             OperandNeedsOverride(policy, path, path_stat));
  }

  if constexpr (Policy::kPrompts) {
    // Wait returns only once no task runs and no request is out, since
    // each holds a WorkToken; then nothing more can be asked.
    std::jthread waiter([&] {
      scheduler.Wait();
      requests.Push(remmy::Drained{});
    });
    remmy::Prompter<Policy, Scheduler> prompter(console, requests, scheduler,
                                                policy, remover());
    prompter.Run();
    waiter.join();
    failures += prompter.Failures();
  } else {
    scheduler.Wait();
  }

  for (auto& worker : scheduler.Workers()) {
    worker.FlushHeld();
    failures += worker.Failures();
  }

  return failures == 0 ? 0 : 1;
}

auto RunRm(const remmy::Cli& cli) -> int {
  remmy::Console console;
  if (cli.Options().prompt_once) {
    if (const auto prompt =
            remmy::BulkPromptFor(cli.Operands(), cli.Options().recursive);
        prompt && console.Ask(*prompt) == remmy::Answer::kNo) {
      return 1;
    }
  }

  return std::visit(
      [&](auto policy) { return RunRmWith(cli, console, policy); },
      remmy::ChoosePolicy(
          cli.Options(),
          remmy::PromptContext{.mode = remmy::CurrentCommandMode(),
                               .stdin_is_tty = ::isatty(STDIN_FILENO) != 0,
                               .is_root = cutils::os::GetEUid() == 0}));
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
