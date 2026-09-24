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

namespace cutils::os::limits::fd {

/// @brief Report the current soft file-descriptor limit, clamped to the
///        per-process ceiling.
[[nodiscard]] auto Get() noexcept -> std::expected<std::uint64_t, std::errc>;

/// @brief Set the soft file-descriptor limit.
[[nodiscard]] auto Set(std::uint64_t soft) noexcept
    -> std::expected<void, std::errc>;

/// @brief Raise the soft limit to the highest value currently permitted.
[[nodiscard]] auto SetMax() noexcept -> std::expected<std::uint64_t, std::errc>;

/// @brief Tracks how many file descriptors this process holds relative to the
///        limit, raising it when leases approach capacity.
class Pool {
 public:
  /// @brief The number of file descriptors currently leased.
  [[nodiscard]] static auto Live() noexcept -> std::uint64_t;

  /// @brief The number of file descriptors we can currently open.
  [[nodiscard]] static auto Capacity() noexcept -> std::uint64_t;

  /// @brief Record that a file descriptor was leased, raising the soft limit
  ///        once the pool crosses its raise threshold.
  static void NoteAcquired() noexcept;

  /// @brief Record that a leased file descriptor was released.
  static void NoteReleased() noexcept;

  /// @brief Record that an open counted by NoteAcquired beforehand failed.
  ///
  /// Unlike NoteReleased, this does not advance the Epoch: the kernel never
  /// handed out a descriptor, so none was freed.
  static void NoteAbandoned() noexcept;

  /// @brief How many leased descriptors have been released so far.
  ///
  /// Snapshot it before an open, for MayHaveFreed.
  [[nodiscard]] static auto Epoch() noexcept -> std::uint64_t;

  /// @brief Whether an open refused for lack of descriptors, attempted after
  ///        `epoch` was taken, could succeed if retried.
  ///
  /// True when one of our descriptors is leased or being opened now, or one
  /// was released since `epoch`: any of them may have held the slot the kernel
  /// refused, and it is or will be freed. False means none of ours held a slot
  /// through the attempt, so the pressure is not ours and waiting cannot help.
  /// Exact only for descriptors counted before their open (see Fd::Open).
  [[nodiscard]] static auto MayHaveFreed(std::uint64_t epoch) noexcept -> bool;

  /// @brief Record that opening a file descriptor was refused, teaching the
  ///        pool where the practical ceiling is.
  static void NoteExhaustion() noexcept;

  /// @brief Whether leases have reached the observed exhaustion ceiling.
  [[nodiscard]] static auto Exhausted() noexcept -> bool;
};

}  // namespace cutils::os::limits::fd

#endif
