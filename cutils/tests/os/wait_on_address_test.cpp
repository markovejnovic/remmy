// See LICENSE in the repository root.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cutils/os/wait_on_address.hpp>
#include <latch>
#include <thread>

namespace {

// Long enough that a started waiter is asleep in the kernel before the wake,
// so the wake, not the value check, is what releases it.
constexpr auto kFallAsleep = std::chrono::milliseconds(20);

/// Sleep on `word` until it stops holding 0, re-checking after each return.
void WaitWhileZero(std::atomic<std::uint32_t>& word) noexcept {
  while (word.load() == 0) {
    cutils::os::WaitOnAddress(word, 0);
  }
}

}  // namespace

SCENARIO("A wait on a word that no longer holds the value returns at once",
         "[wait_on_address]") {
  GIVEN("a word holding 1") {
    std::atomic<std::uint32_t> word{1};

    WHEN("a thread waits while it holds 0") {
      cutils::os::WaitOnAddress(word, 0);

      THEN("the wait returns without anyone waking it") { SUCCEED(); }
    }
  }
}

SCENARIO("WakeOne releases a thread asleep on the word", "[wait_on_address]") {
  GIVEN("a thread asleep while the word holds 0") {
    std::atomic<std::uint32_t> word{0};
    std::latch started{1};
    std::jthread waiter([&] {
      started.count_down();
      WaitWhileZero(word);
    });
    started.wait();
    std::this_thread::sleep_for(kFallAsleep);

    WHEN("the word changes and one sleeper is woken") {
      word.store(1);
      cutils::os::WakeOne(word);
      waiter.join();

      THEN("the sleeper returned") { CHECK(word.load() == 1); }
    }
  }
}

SCENARIO("WakeAll releases every thread asleep on the word",
         "[wait_on_address]") {
  GIVEN("four threads asleep while the word holds 0") {
    constexpr std::size_t kSleepers = 4;
    std::atomic<std::uint32_t> word{0};
    std::latch started{kSleepers};
    std::array<std::jthread, kSleepers> sleepers;
    for (auto& sleeper : sleepers) {
      sleeper = std::jthread([&] {
        started.count_down();
        WaitWhileZero(word);
      });
    }
    started.wait();
    std::this_thread::sleep_for(kFallAsleep);

    WHEN("the word changes and all sleepers are woken") {
      word.store(1);
      cutils::os::WakeAll(word);
      for (auto& sleeper : sleepers) {
        sleeper.join();
      }

      THEN("every sleeper returned") { CHECK(word.load() == 1); }
    }
  }
}
