// See LICENSE in the repository root.

#include "cutils/os/os.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace cutils::os {
namespace {

[[nodiscard, maybe_unused]] auto TakeStrError(int result,
                                              std::string& text) noexcept
    -> std::optional<std::string_view> {
  if (result == ERANGE) {
    return std::nullopt;
  }

  text.resize(std::strlen(text.c_str()));
  return text;
}

[[nodiscard, maybe_unused]] auto TakeStrError(const char* result,
                                              std::string& text) noexcept
    -> std::optional<std::string_view> {
  if (result != text.data()) {
    return result;
  }
  text.resize(std::strlen(text.c_str()));
  return text;
}

}  // namespace

auto StrError(int error) noexcept -> std::string_view {
  static constexpr std::size_t kInitialBytes = 128;
  static constexpr std::size_t kMaxBytes = 64UZ * 1024UZ;

  thread_local std::string text;
  text.resize(text.capacity() > kInitialBytes ? text.capacity()
                                              : kInitialBytes);

  while (true) {
    if (const auto taken =
            TakeStrError(::strerror_r(error, text.data(), text.size()), text)) {
      return *taken;
    }

    if (text.size() >= kMaxBytes) {
      text.resize(std::strlen(text.c_str()));
      return text;
    }

    text.resize(text.size() * 2);
  }
}

}  // namespace cutils::os
