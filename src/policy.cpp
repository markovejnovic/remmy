// See LICENSE in the repository root.

#include "policy.hpp"

#include <strings.h>

#include <cstdlib>

namespace remmy {

auto CurrentCommandMode() noexcept -> CommandMode {
  const char* mode = std::getenv("COMMAND_MODE");
  return mode != nullptr && ::strcasecmp(mode, "legacy") == 0
             ? CommandMode::kLegacy
             : CommandMode::kUnix2003;
}

auto ChoosePolicy(const Options& options, PromptContext context) noexcept
    -> PromptPolicy {
  if (options.interactive) {
    return AskAll{.mode = context.mode};
  }
  if (!options.force && context.stdin_is_tty) {
    return AskUnwritable{.mode = context.mode, .is_root = context.is_root};
  }
  return NeverAsk{};
}

}  // namespace remmy
