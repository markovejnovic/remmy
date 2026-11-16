// See LICENSE in the repository root.

#include "cutils/os/wait_on_address.hpp"

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#endif

#include <atomic>
#include <cstdint>

namespace cutils::os {

#if defined(__APPLE__)

void WaitOnAddress(std::atomic<std::uint32_t>& word,
                   std::uint32_t expected) noexcept {
  // EINTR and a value that already changed both mean "re-check", which every
  // caller does anyway.
  (void)::os_sync_wait_on_address(&word, expected, sizeof(word),
                                  OS_SYNC_WAIT_ON_ADDRESS_NONE);
}

void WakeOne(std::atomic<std::uint32_t>& word) noexcept {
  // ENOENT: nobody was asleep, which is fine.
  (void)::os_sync_wake_by_address_any(&word, sizeof(word),
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

void WakeAll(std::atomic<std::uint32_t>& word) noexcept {
  (void)::os_sync_wake_by_address_all(&word, sizeof(word),
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

#else

void WaitOnAddress(std::atomic<std::uint32_t>& word,
                   std::uint32_t expected) noexcept {
  (void)::syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, expected, nullptr,
                  nullptr, 0);
}

void WakeOne(std::atomic<std::uint32_t>& word) noexcept {
  (void)::syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void WakeAll(std::atomic<std::uint32_t>& word) noexcept {
  (void)::syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr,
                  nullptr, 0);
}

#endif

}  // namespace cutils::os
