// See LICENSE in the repository root.

/// @brief Utilities for interfacing with exceptions.
#ifndef CUTILS_EXCEPTIONS_EXCEPTIONS_HPP
#define CUTILS_EXCEPTIONS_EXCEPTIONS_HPP

#if defined(__cpp_exceptions)
#include <utility>
#else
#include <cstdlib>
#endif

namespace cutils {

/// @brief Throw `Exception{args...}` when exceptions are enabled, else abort.
template <typename Exception, typename... Args>
[[noreturn]] constexpr void ThrowOrAbort([[maybe_unused]] Args&&... args) {
#if defined(__cpp_exceptions)
  throw Exception(std::forward<Args>(args)...);
#else
  std::abort();
#endif
}

}  // namespace cutils

#endif  // CUTILS_EXCEPTIONS_EXCEPTIONS_HPP
