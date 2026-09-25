// See LICENSE in the repository root.

#include "cutils/os/limits/fd.hpp"

#include <sys/resource.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <system_error>

namespace cutils::os::limits::fd {
namespace {

// Leased descriptors, and how many have been released so far (the epoch).
struct Leases {
  std::uint32_t live;
  std::uint32_t epoch;
};

// Leases packed into one atomic word, live in the low half and the epoch in
// the high half: a release is one read-modify-write that both frees a lease
// and advances the epoch, and a single load sees both at once.
class AtomicLeases {
 public:
  [[nodiscard]] auto Load() const noexcept -> Leases {
    return Unpack(bits_.load(std::memory_order_relaxed));
  }

  /// Returns the leases as they stand after this acquire.
  auto Acquire() noexcept -> Leases {
    return Unpack(bits_.fetch_add(kAcquire, std::memory_order_relaxed) +
                  kAcquire);
  }

  void Release() noexcept {
    bits_.fetch_add(kRelease, std::memory_order_relaxed);
  }

  void Abandon() noexcept {
    bits_.fetch_sub(kAcquire, std::memory_order_relaxed);
  }

 private:
  static constexpr int kEpochShift = 32;
  static constexpr std::uint64_t kAcquire = 1;
  // Adding this drops the live count by one and carries one into the epoch.
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
  int value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname("kern.maxfilesperproc", &value, &size, nullptr, 0) != 0) {
    return std::unexpected(static_cast<std::errc>(errno));
  }
  if (value <= 0) {
    return std::unexpected(std::errc::invalid_argument);
  }
  return static_cast<std::uint64_t>(value);
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
  return leases.Load().live;
}

auto Pool::Capacity() noexcept -> std::uint64_t { return CapacityImpl(); }

void Pool::NoteAcquired() noexcept { RaiseIfNeeded(leases.Acquire().live); }

void Pool::NoteReleased() noexcept { leases.Release(); }

void Pool::NoteAbandoned() noexcept { leases.Abandon(); }

auto Pool::Epoch() noexcept -> std::uint64_t { return leases.Load().epoch; }

auto Pool::MayHaveFreed(std::uint64_t epoch) noexcept -> bool {
  // Relaxed suffices: a descriptor that held the refused slot was counted
  // before its open, the kernel ordered that open before the refusal, and the
  // caller loads after the refusal. Coherence of the one atomic then shows the
  // lease, or a later state whose epoch has moved past it.
  const auto now = leases.Load();
  return now.live != 0 || now.epoch != epoch;
}

void Pool::NoteExhaustion() noexcept {
  const auto live = Live();
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
