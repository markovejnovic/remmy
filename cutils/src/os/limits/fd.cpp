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

class AtomicLeases {
 public:
  [[nodiscard]] auto Load() const noexcept -> Leases {
    return Unpack(bits_.load(std::memory_order_relaxed));
  }

  auto Acquire() noexcept -> Leases {
    return Unpack(bits_.fetch_add(kAcquire, std::memory_order_relaxed));
  }

  void Release() noexcept {
    bits_.fetch_add(kRelease, std::memory_order_relaxed);
  }

  void Forget() noexcept {
    bits_.fetch_sub(kAcquire, std::memory_order_relaxed);
  }

 private:
  static constexpr unsigned kEpochShift = 32;
  static constexpr std::uint64_t kAcquire = 1;
  static constexpr std::uint64_t kRelease =
      (std::uint64_t{1} << kEpochShift) - kAcquire;

  [[nodiscard]] static constexpr auto Unpack(std::uint64_t bits) noexcept
      -> Leases {
    return {.live = static_cast<std::uint32_t>(bits),
            .epoch = static_cast<std::uint32_t>(bits >> kEpochShift)};
  }

  std::atomic<std::uint64_t> bits_{0};
};

AtomicLeases leases;

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

auto Pool::Capacity() noexcept -> std::uint64_t { return CapacityImpl(); }

auto Pool::Exhausted() noexcept -> bool {
  const auto ceiling = observed_ceiling.load(std::memory_order_relaxed);
  return ceiling != 0 && leases.Load().live >= ceiling;
}

auto Pool::Reserve() noexcept -> Reservation {
  const auto before = leases.Acquire();
  RaiseIfNeeded(std::uint64_t{before.live} + 1);
  return Reservation{before};
}

void Pool::Release() noexcept { leases.Release(); }

void Pool::Forget() noexcept { leases.Forget(); }

Pool::Reservation::~Reservation() {
  if (armed_) {
    leases.Forget();  // The open failed: no slot was handed out, none freed.
  }
}

void Pool::Reservation::Claim() && noexcept { armed_ = false; }

auto Pool::Reservation::Refused() && noexcept -> bool {
  armed_ = false;
  leases.Forget();

  if (const std::uint64_t held = before_.live; held != 0) {
    auto ceiling = observed_ceiling.load(std::memory_order_relaxed);
    while ((ceiling == 0 || held < ceiling) &&
           !observed_ceiling.compare_exchange_weak(ceiling, held,
                                                   std::memory_order_relaxed)) {
    }
  }

  const auto now = leases.Load();
  return now.live != 0 || now.epoch != before_.epoch;
}

}  // namespace cutils::os::limits::fd
