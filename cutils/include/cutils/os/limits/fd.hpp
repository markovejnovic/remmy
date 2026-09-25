// See LICENSE in the repository root.

/// @brief Utilities for operating on file descriptor limits.
#ifndef CUTILS_OS_LIMITS_FD_HPP
#define CUTILS_OS_LIMITS_FD_HPP

#ifndef __APPLE__
#error "cutils/os/limits/fd.hpp: fd limit clamping requires Darwin"
#endif

#include <cstdint>
#include <expected>
#include <system_error>

namespace cutils::os {
class Fd;
}  // namespace cutils::os

namespace cutils::os::limits::fd {

/// @brief Report the current soft file-descriptor limit, clamped to the
///        per-process ceiling.
[[nodiscard]] auto Get() noexcept -> std::expected<std::uint64_t, std::errc>;

/// @brief Set the soft file-descriptor limit.
[[nodiscard]] auto Set(std::uint64_t soft) noexcept
    -> std::expected<void, std::errc>;

/// @brief Raise the soft limit to the highest value currently permitted.
[[nodiscard]] auto SetMax() noexcept -> std::expected<std::uint64_t, std::errc>;

/// @brief Leased descriptors, and how many have been released so far.
struct Leases {
  std::uint32_t live;
  std::uint32_t epoch;
};

/// @brief Tracks how many file descriptors this process holds relative to the
///        limit, raising it when leases approach capacity.
class Pool {
 public:
  /// @brief The number of file descriptors we can currently open.
  [[nodiscard]] static auto Capacity() noexcept -> std::uint64_t;

  /// @brief Whether leases have reached the observed exhaustion ceiling, so an
  ///        open now would likely be refused.
  [[nodiscard]] static auto Exhausted() noexcept -> bool;

 private:
  friend class cutils::os::Fd;

  /// @brief A lease taken ahead of an open. Claimed by the Fd it opens, or
  ///        consumed by Refused; otherwise dropped on destruction, freeing no
  ///        slot.
  class [[nodiscard]] Reservation {
   public:
    Reservation(const Reservation&) = delete;
    Reservation(Reservation&&) = delete;
    auto operator=(const Reservation&) -> Reservation& = delete;
    auto operator=(Reservation&&) -> Reservation& = delete;
    ~Reservation();

    /// @brief Hand the lease to the descriptor that was opened.
    void Claim() && noexcept;

    /// @brief Record that the open was refused for lack of descriptors.
    ///
    /// @return Whether a retry could succeed: one of ours is leased now, or
    ///         was released since this reservation, so may have held the slot.
    auto Refused() && noexcept -> bool;

   private:
    friend class Pool;
    explicit Reservation(Leases before) noexcept : before_(before) {}

    Leases before_;
    bool armed_ = true;
  };

  /// @brief Lease a descriptor ahead of opening it, raising the soft limit
  ///        once the pool crosses its raise threshold.
  static auto Reserve() noexcept -> Reservation;

  /// @brief Return a lease whose descriptor was closed, advancing the epoch.
  static void Release() noexcept;

  /// @brief Return a lease whose descriptor was handed off still open. The
  ///        epoch stays put.
  static void Forget() noexcept;
};

}  // namespace cutils::os::limits::fd

#endif
