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

auto ChoosePolicy(const Options& options, CommandMode mode) noexcept
    -> PromptPolicy {
  if (options.interactive) {
    return AskAll{.mode = mode};
  }
  return NeverAsk{};
}

}  // namespace remmy
