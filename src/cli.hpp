// See LICENSE in the repository root.

#ifndef REMMY_CLI_HPP
#define REMMY_CLI_HPP

#include <cutils/clppap/clppap.hpp>

namespace remmy {

struct[[= cpplap::Help("remmy: rm alternative")]] Cli {
  [[
    = cpplap::Short("-r"), = cpplap::Long("--recursive")
  ]][[= cpplap::Help("Remove directories recursively")]] bool is_recursive =
      false;

  [[
    = cpplap::Short("-h"), = cpplap::Long("--help")
  ]][[ = cpplap::Help("Show this help"), = cpplap::HelpFlag ]] bool help =
      false;

  [[ = cpplap::Positional,
     = cpplap::Help("Paths to remove") ]] cpplap::PositionalIterator positional;
};

}  // namespace remmy

#endif
