// See LICENSE in the repository root.

/// @brief When rm asks before removing something.
///
/// Chosen once per run from the options and the environment; the workers
/// are compiled per policy, so a run that never asks pays nothing for asking.
#ifndef REMMY_POLICY_HPP
#define REMMY_POLICY_HPP

#include <sys/stat.h>

#include <cstdint>
#include <variant>

#include "cli.hpp"
#include "cutils/os/attr_dir_reader.hpp"
#include "cutils/sync/output_gate.hpp"
#include "cutils/task_scheduler/task_scheduler.hpp"
#include "dir_node.hpp"

namespace remmy {

/// @brief libc's compat_mode("bin/rm", ...), from COMMAND_MODE.
enum class CommandMode : std::uint8_t {
  /// The default: -ir asks to examine a directory, then to remove it.
  kUnix2003,
  /// COMMAND_MODE=legacy: -ir asks once, to remove it, before entering.
  kLegacy,
};

/// @brief Never ask: -f, or neither -i nor a terminal to ask on.
struct NeverAsk {
  using Gate = cutils::sync::NoGate;
  static constexpr cutils::Idle kIdle = cutils::Idle::kSpin;

  /// @brief Whether anything may be asked, so a prompter must run.
  static constexpr bool kPrompts = false;

  /// @brief Whether every entry is asked about before it is removed.
  static constexpr bool kAsksEveryEntry = false;

  /// @brief Whether walks read each entry's flags and writability.
  static constexpr bool kReadsAttrs = false;

  /// @brief Whether the emptied `dir` is asked about before its rmdir.
  [[nodiscard]] static constexpr auto AsksBeforeRmdir(
      const DirNode& /*dir*/) noexcept -> bool {
    return false;
  }
};

/// @brief -i: ask before every removal and before entering each directory.
struct AskAll {
  using Gate = cutils::sync::OutputGate;
  static constexpr cutils::Idle kIdle = cutils::Idle::kPark;
  static constexpr bool kPrompts = true;
  static constexpr bool kAsksEveryEntry = true;
  static constexpr bool kReadsAttrs = false;

  /// @brief Legacy mode asks once, before entering; the default asks again
  ///        before the rmdir.
  [[nodiscard]] constexpr auto AsksBeforeRmdir(
      const DirNode& /*dir*/) const noexcept -> bool {
    return mode == CommandMode::kUnix2003;
  }

  /// @brief Whether entering is asked as "examine files in directory d? "
  ///        (the default) rather than legacy's "remove d? ".
  [[nodiscard]] constexpr auto AsksToExamine() const noexcept -> bool {
    return mode == CommandMode::kUnix2003;
  }

  CommandMode mode;
};

/// @brief Neither -f nor -i, and a terminal on stdin: ask before removing
///        what this process cannot write, or what file flags protect.
struct AskUnwritable {
  using Gate = cutils::sync::OutputGate;
  static constexpr cutils::Idle kIdle = cutils::Idle::kPark;
  static constexpr bool kPrompts = true;
  static constexpr bool kAsksEveryEntry = false;
  static constexpr bool kReadsAttrs = true;

  [[nodiscard]] static constexpr auto AsksBeforeRmdir(
      const DirNode& dir) noexcept -> bool {
    return dir.needs_override_;
  }

  /// @brief rm's check(): whether an entry gets the override prompt.
  [[nodiscard]] constexpr auto NeedsOverride(cutils::os::EntryKind kind,
                                             bool writable,
                                             std::uint32_t flags) const noexcept
      -> bool {
#if defined(__APPLE__)
    constexpr std::uint32_t kSystemFlags = SF_APPEND | SF_IMMUTABLE;
    constexpr std::uint32_t kUserFlags = UF_APPEND | UF_IMMUTABLE;
#else
    constexpr std::uint32_t kSystemFlags = 0;
    constexpr std::uint32_t kUserFlags = 0;
#endif
    // A symlink's permissions mean nothing; root may clear user flags.
    return kind != cutils::os::EntryKind::kSymlink &&
           (!writable || (flags & kSystemFlags) != 0 ||
            (!is_root && (flags & kUserFlags) != 0));
  }

  /// @brief How walks read attributes. Root reads them by access(2) itself,
  ///        whose answers for root getattrlistbulk need not share.
  [[nodiscard]] constexpr auto ReadMethod() const noexcept
      -> cutils::os::AttrDirReader::Method {
    return is_root ? cutils::os::AttrDirReader::Method::kPerEntry
                   : cutils::os::AttrDirReader::Method::kBulk;
  }

  bool is_root;
};

using PromptPolicy = std::variant<NeverAsk, AskAll, AskUnwritable>;

/// @brief The mode COMMAND_MODE selects, as libc reads it.
[[nodiscard]] auto CurrentCommandMode() noexcept -> CommandMode;

/// @brief What rm's prompts depend on beyond the options.
struct PromptContext {
  CommandMode mode;
  bool stdin_is_tty;
  bool is_root;
};

/// @brief The policy rm follows for `options`.
[[nodiscard]] auto ChoosePolicy(const Options& options,
                                PromptContext context) noexcept -> PromptPolicy;

}  // namespace remmy

#endif
