// See LICENSE in the repository root.

/// @brief When rm asks before removing something.
///
/// Chosen once per run from the options and the environment; the workers
/// are compiled per policy, so a run that never asks pays nothing for asking.
#ifndef REMMY_POLICY_HPP
#define REMMY_POLICY_HPP

#include <cstdint>
#include <variant>

#include "cli.hpp"
#include "cutils/sync/output_gate.hpp"
#include "cutils/task_scheduler/task_scheduler.hpp"

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

  /// @brief Whether an emptied directory is asked about before its rmdir.
  [[nodiscard]] static constexpr auto AsksBeforeRmdir() noexcept -> bool {
    return false;
  }
};

/// @brief -i: ask before every removal and before entering each directory.
struct AskAll {
  using Gate = cutils::sync::OutputGate;
  static constexpr cutils::Idle kIdle = cutils::Idle::kPark;
  static constexpr bool kPrompts = true;
  static constexpr bool kAsksEveryEntry = true;

  /// @brief Legacy mode asks once, before entering; the default asks again
  ///        before the rmdir.
  [[nodiscard]] constexpr auto AsksBeforeRmdir() const noexcept -> bool {
    return mode == CommandMode::kUnix2003;
  }

  /// @brief Whether entering is asked as "examine files in directory d? "
  ///        (the default) rather than legacy's "remove d? ".
  [[nodiscard]] constexpr auto AsksToExamine() const noexcept -> bool {
    return mode == CommandMode::kUnix2003;
  }

  CommandMode mode;
};

using PromptPolicy = std::variant<NeverAsk, AskAll>;

/// @brief The mode COMMAND_MODE selects, as libc reads it.
[[nodiscard]] auto CurrentCommandMode() noexcept -> CommandMode;

/// @brief The policy rm follows for `options`.
[[nodiscard]] auto ChoosePolicy(const Options& options,
                                CommandMode mode) noexcept -> PromptPolicy;

}  // namespace remmy

#endif
