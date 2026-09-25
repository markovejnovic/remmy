// See LICENSE in the repository root.

#include "cutils/os/limits/fd.hpp"

#include <sys/resource.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <system_error>

namespace cutils::os::limits::fd {
namespace {

std::atomic<std::uint64_t> live_fds{0};
std::atomic<std::uint64_t> known_capacity{0};
std::atomic<std::uint64_t> observed_ceiling{0};
std::atomic_flag raise_attempted = ATOMIC_FLAG_INIT;

[[nodiscard]] auto MaxFilesPerProc() noexcept
    -> std::expected<std::uint64_t, std::errc> {
#if defined(__linux__)
  // The kernel never lets the hard limit exceed fs.nr_open, so the rlimit
  // already carries the per-process ceiling.
  return std::numeric_limits<std::uint64_t>::max();
#else
  int value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname("kern.maxfilesperproc", &value, &size, nullptr, 0) != 0) {
    return std::unexpected(static_cast<std::errc>(errno));
  }
  if (value <= 0) {
    return std::unexpected(std::errc::invalid_argument);
  }
  return static_cast<std::uint64_t>(value);
#endif
}

[[nodiscard]] auto Rlimit() noexcept -> std::expected<::rlimit, std::errc> {
  ::rlimit current{};
  if (::getrlimit(RLIMIT_NOFILE, &current) != 0) {
    return std::unexpected(static_cast<std::errc>(errno));
  }
  return current;
}

[[nodiscard]] auto CapacityImpl() noexcept -> std::uint64_t {
  auto cached = known_capacity.load(std::memory_order_relaxed);
  if (cached == 0) {
    cached = Get().value_or(0);
    known_capacity.store(cached, std::memory_order_relaxed);
  }
  return cached;
}

// Raise once, when leases reach kRaiseAtPercent of what we can currently open.
void RaiseIfNeeded(std::uint64_t live) noexcept {
  static constexpr std::uint64_t kRaiseAtPercent = 50;

  const auto capacity = CapacityImpl();
  if (capacity == 0 || live * 100 < capacity * kRaiseAtPercent) {
    return;
  }
  if (raise_attempted.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  if (const auto raised = SetMax()) {
    known_capacity.store(*raised, std::memory_order_relaxed);
    observed_ceiling.store(0, std::memory_order_relaxed);  // Room again.
  }
}

}  // namespace

auto Get() noexcept -> std::expected<std::uint64_t, std::errc> {
  const auto current = Rlimit();
  if (!current) {
    return std::unexpected(current.error());
  }
  const auto cap = MaxFilesPerProc();
  if (!cap) {
    return std::unexpected(cap.error());
  }
  return std::min(static_cast<std::uint64_t>(current->rlim_cur), *cap);
}

auto Set(std::uint64_t soft) noexcept -> std::expected<void, std::errc> {
  const auto current = Rlimit();
  if (!current) {
    return std::unexpected(current.error());
  }
  ::rlimit updated = *current;
  updated.rlim_cur = static_cast<::rlim_t>(soft);
  if (::setrlimit(RLIMIT_NOFILE, &updated) != 0) {
    return std::unexpected(static_cast<std::errc>(errno));
  }
  return {};
}

auto SetMax() noexcept -> std::expected<std::uint64_t, std::errc> {
  const auto current = Rlimit();
  if (!current) {
    return std::unexpected(current.error());
  }
  const auto cap = MaxFilesPerProc();
  if (!cap) {
    return std::unexpected(cap.error());
  }
  const std::uint64_t target =
      std::min(static_cast<std::uint64_t>(current->rlim_max), *cap);
  if (const auto applied = Set(target); !applied) {
    return std::unexpected(applied.error());
  }
  return target;
}

auto Pool::Live() noexcept -> std::uint64_t {
  return live_fds.load(std::memory_order_relaxed);
}

auto Pool::Capacity() noexcept -> std::uint64_t { return CapacityImpl(); }

void Pool::NoteAcquired() noexcept {
  const auto live = live_fds.fetch_add(1, std::memory_order_relaxed) + 1;
  RaiseIfNeeded(live);
}

void Pool::NoteReleased() noexcept {
  live_fds.fetch_sub(1, std::memory_order_relaxed);
}

void Pool::NoteExhaustion() noexcept {
  const auto live = live_fds.load(std::memory_order_relaxed);
  if (live == 0) {
    return;  // Nothing of ours was open; the pressure is not ours to model.
  }
  auto ceiling = observed_ceiling.load(std::memory_order_relaxed);
  while ((ceiling == 0 || live < ceiling) &&
         !observed_ceiling.compare_exchange_weak(ceiling, live,
                                                 std::memory_order_relaxed)) {
  }
}

auto Pool::Exhausted() noexcept -> bool {
  const auto ceiling = observed_ceiling.load(std::memory_order_relaxed);
  return ceiling != 0 && Live() >= ceiling;
}

}  // namespace cutils::os::limits::fd
