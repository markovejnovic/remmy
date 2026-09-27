// See LICENSE in the repository root.

#ifndef CUTILS_VARIANT_HPP
#define CUTILS_VARIANT_HPP

namespace cutils::variant {

template <typename... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};

}  // namespace cutils::variant

#endif  // CUTILS_VARIANT_HPP
