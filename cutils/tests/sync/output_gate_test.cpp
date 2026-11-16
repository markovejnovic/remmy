// See LICENSE in the repository root.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cutils/sync/output_gate.hpp>
#include <thread>

namespace {

// Long enough for a Close that is going to wrongly return to have returned.
constexpr auto kSettle = std::chrono::milliseconds(20);

}  // namespace

SCENARIO("A closed gate turns writers away until it opens", "[output_gate]") {
  GIVEN("a gate") {
    cutils::sync::OutputGate gate;

    WHEN("it is closed") {
      gate.Close();

      THEN("a writer cannot enter") { CHECK_FALSE(gate.TryEnter()); }

      AND_WHEN("it opens again") {
        gate.Open();

        THEN("a writer can enter") {
          CHECK(gate.TryEnter());
          gate.Leave();
        }
      }
    }
  }
}

SCENARIO("Closing waits for the writers already inside", "[output_gate]") {
  GIVEN("a writer inside the gate and a closer waiting on it") {
    cutils::sync::OutputGate gate;
    REQUIRE(gate.TryEnter());
    std::atomic<bool> closed{false};
    std::jthread closer([&] {
      gate.Close();
      closed.store(true);
    });
    std::this_thread::sleep_for(kSettle);

    THEN("the gate is not closed while the writer is inside") {
      CHECK_FALSE(closed.load());
      CHECK_FALSE(gate.TryEnter());
    }

    WHEN("the writer leaves") {
      gate.Leave();
      closer.join();

      THEN("closing completes") { CHECK(closed.load()); }
    }

    // The closer's join, on scope exit, needs the writer gone.
    if (!closed.load()) {
      gate.Leave();
    }
  }
}

SCENARIO("A gate that never closes always lets writers in", "[output_gate]") {
  STATIC_REQUIRE(cutils::sync::NoGate::TryEnter());
}
