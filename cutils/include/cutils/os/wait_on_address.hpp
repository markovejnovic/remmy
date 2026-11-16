// See LICENSE in the repository root.

/// @brief Sleeping on a 32-bit word until another thread changes it.
///
/// These go straight to futex(2) on Linux and os_sync_wait_on_address(3) on
/// macOS. std::atomic<T>::wait may route through libstdc++'s shared proxy
/// table, whose locks a waker on a hot path must not take.
#ifndef CUTILS_OS_WAIT_ON_ADDRESS_HPP
#define CUTILS_OS_WAIT_ON_ADDRESS_HPP

#include <atomic>
#include <cstdint>

namespace cutils::os {

static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t) &&
                  std::atomic<std::uint32_t>::is_always_lock_free,
              "the kernel waits on the atomic's own four bytes");

/// @brief Sleep while `word` holds `expected`.
///
/// Returns once woken, once `word` no longer holds `expected`, or spuriously,
/// so callers re-check their condition in a loop.
void WaitOnAddress(std::atomic<std::uint32_t>& word,
                   std::uint32_t expected) noexcept;

/// @brief Wake one thread asleep in WaitOnAddress on `word`, if there is one.
void WakeOne(std::atomic<std::uint32_t>& word) noexcept;

/// @brief Wake every thread asleep in WaitOnAddress on `word`.
void WakeAll(std::atomic<std::uint32_t>& word) noexcept;

}  // namespace cutils::os

#endif  // CUTILS_OS_WAIT_ON_ADDRESS_HPP
