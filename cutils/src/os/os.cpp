// See LICENSE in the repository root.

#include "cutils/os/os.hpp"

#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

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

auto GetEUid() noexcept -> ::uid_t {
  static constexpr auto kUnloaded = static_cast<::uid_t>(-1);
  static std::atomic<::uid_t> cached{kUnloaded};

  ::uid_t euid = cached.load(std::memory_order_relaxed);
  if (euid == kUnloaded) {
    euid = ::geteuid();
    cached.store(euid, std::memory_order_relaxed);
  }
  return euid;
}

auto PerformanceCores() noexcept -> unsigned {
#if defined(__APPLE__)
  int cores = 0;
  std::size_t size = sizeof(cores);
  if (::sysctlbyname("hw.perflevel0.logicalcpu", &cores, &size, nullptr, 0) ==
          0 &&
      cores > 0) {
    return static_cast<unsigned>(cores);
  }
#endif
  return std::max(1U, std::thread::hardware_concurrency());
}

void MarkThreadUserInitiated() noexcept {
#if defined(__APPLE__)
  // Best effort: the only failure is an invalid class.
  std::ignore = ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

}  // namespace cutils::os
