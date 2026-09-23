// See LICENSE in the repository root.

#ifndef CUTILS_CONCEPTS_HPP
#define CUTILS_CONCEPTS_HPP

#include <type_traits>

namespace cutils {

template <typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

}  // namespace cutils

#endif  // CUTILS_CONCEPTS_HPP
