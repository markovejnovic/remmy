// See LICENSE in the repository root.

/// @brief A gate one thread closes to get output to itself, without ever
///        making the many writers behind it wait.
///
/// Writers TryEnter around each write and do something else (such as buffer
/// it) when the gate is closed. Close waits only for the writes already in
/// flight, then holds until Open.
#ifndef CUTILS_SYNC_OUTPUT_GATE_HPP
#define CUTILS_SYNC_OUTPUT_GATE_HPP

#include <atomic>
#include <cstdint>

#include "cutils/os/wait_on_address.hpp"

namespace cutils::sync {

/// @brief Many writers at once, one closer; only the closer ever waits.
class OutputGate {
 public:
  OutputGate() noexcept = default;
  OutputGate(const OutputGate&) = delete;
  OutputGate(OutputGate&&) = delete;
  auto operator=(const OutputGate&) -> OutputGate& = delete;
  auto operator=(OutputGate&&) -> OutputGate& = delete;
  ~OutputGate() = default;

  /// @brief Enter to write, or false at once when the gate is closed.
  [[nodiscard]] auto TryEnter() noexcept -> bool {
    std::uint32_t state = state_.load(std::memory_order_relaxed);
    do {
      if ((state & kClosed) != 0) {
        return false;
      }
    } while (!state_.compare_exchange_weak(state, state + 1,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed));
    return true;
  }

  /// @brief Leave after a successful TryEnter.
  void Leave() noexcept {
    if (state_.fetch_sub(1, std::memory_order_release) == (kClosed | 1U)) {
      os::WakeOne(state_);
    }
  }

  /// @brief Turn new writers away and wait for those inside to leave. One
  ///        thread closes at a time.
  void Close() noexcept {
    std::uint32_t state =
        state_.fetch_or(kClosed, std::memory_order_acq_rel) | kClosed;
    while (state != kClosed) {
      os::WaitOnAddress(state_, state);
      state = state_.load(std::memory_order_acquire);
    }
  }

  /// @brief Let writers in again.
  void Open() noexcept {
    state_.fetch_and(~kClosed, std::memory_order_release);
  }

 private:
  static constexpr std::uint32_t kClosed = 1U << 31U;

  /// @brief kClosed, plus how many writers are inside.
  std::atomic<std::uint32_t> state_{0};
};

/// @brief An OutputGate that never closes, for runs that never prompt.
struct NoGate {
  [[nodiscard]] static constexpr auto TryEnter() noexcept -> bool {
    return true;
  }

  static constexpr void Leave() noexcept {}
};

}  // namespace cutils::sync

#endif  // CUTILS_SYNC_OUTPUT_GATE_HPP
