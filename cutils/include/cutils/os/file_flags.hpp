// See LICENSE in the repository root.

#ifndef CUTILS_OS_FILE_FLAGS_HPP
#define CUTILS_OS_FILE_FLAGS_HPP

#include <cstdint>
#include <string>

namespace cutils::os {

/// @brief chflags(2) flags by name, comma separated, as rm and `ls -lo` print
///        them, such as `uappnd,uchg`. Empty on Linux, which has none.
[[nodiscard]] auto FlagNames(std::uint32_t flags) -> std::string;

}  // namespace cutils::os

#endif  // CUTILS_OS_FILE_FLAGS_HPP
