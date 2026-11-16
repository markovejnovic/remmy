// See LICENSE in the repository root.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cutils/sync/mpsc_list.hpp>
#include <latch>
#include <thread>
#include <vector>

namespace {

// Long enough that a started consumer is asleep before the push wakes it.
constexpr auto kFallAsleep = std::chrono::milliseconds(20);

/// Which producer pushed a value, and how many it had pushed before.
struct Stamp {
  std::uint32_t producer;
  std::uint32_t seq;
};

/// Counts its live instances, to show which nodes a list has freed.
struct Tracked {
  inline static std::atomic<int> live{0};

  explicit Tracked(int v) noexcept : value(v) { live.fetch_add(1); }
  Tracked(const Tracked& other) noexcept : value(other.value) {
    live.fetch_add(1);
  }
  Tracked(Tracked&& other) noexcept : value(other.value) { live.fetch_add(1); }
  auto operator=(const Tracked&) -> Tracked& = delete;
  auto operator=(Tracked&&) -> Tracked& = delete;
  ~Tracked() { live.fetch_sub(1); }

  int value;
};

/// Take batches until `count` values have arrived, in arrival order.
template <class T>
auto TakeExactly(cutils::sync::MpscList<T>& list, std::size_t count)
    -> std::vector<T> {
  std::vector<T> out;
  while (out.size() < count) {
    for (T& value : list.TakeAll()) {
      out.push_back(value);
    }
  }
  return out;
}

}  // namespace

SCENARIO("Values come back in the order one producer pushed them",
         "[mpsc_list]") {
  GIVEN("a list one thread pushes to across two batches") {
    cutils::sync::MpscList<int> list;
    std::vector<int> taken;

    WHEN("it pushes, the consumer takes, and it pushes again") {
      for (int v = 1; v <= 5; ++v) {
        list.Push(v);
      }
      for (int& v : list.TakeAll()) {
        taken.push_back(v);
      }
      for (int v = 6; v <= 8; ++v) {
        list.Push(v);
      }
      for (int& v : list.TakeAll()) {
        taken.push_back(v);
      }

      THEN("every value arrives once, oldest first") {
        CHECK(taken == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
      }
    }
  }
}

SCENARIO("Concurrent producers each keep their own order", "[mpsc_list]") {
  GIVEN("four producers pushing ten thousand stamped values each") {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kEach = 10'000;
    cutils::sync::MpscList<Stamp> list;
    std::latch go{kProducers};
    std::array<std::jthread, kProducers> producers;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
      producers[p] = std::jthread([&list, &go, p] {
        go.arrive_and_wait();
        for (std::uint32_t seq = 0; seq < kEach; ++seq) {
          list.Push(Stamp{.producer = p, .seq = seq});
        }
      });
    }

    WHEN("the consumer takes everything as it arrives") {
      const std::vector<Stamp> taken =
          TakeExactly(list, std::size_t{kProducers} * kEach);

      THEN("each producer's values arrive exactly once and in its order") {
        std::array<std::uint32_t, kProducers> next{};
        bool in_order = true;
        for (const Stamp& stamp : taken) {
          in_order = in_order && stamp.seq == next[stamp.producer];
          ++next[stamp.producer];
        }
        CHECK(in_order);
        for (const std::uint32_t count : next) {
          CHECK(count == kEach);
        }
      }
    }
  }
}

SCENARIO("A consumer asleep on an empty list wakes for a push", "[mpsc_list]") {
  GIVEN("a consumer blocked taking from an empty list") {
    cutils::sync::MpscList<int> list;
    std::latch started{1};
    std::vector<int> taken;
    std::jthread consumer([&] {
      started.count_down();
      taken = TakeExactly(list, 1);
    });
    started.wait();
    std::this_thread::sleep_for(kFallAsleep);

    WHEN("a value is pushed") {
      list.Push(42);
      consumer.join();

      THEN("the consumer took it") { CHECK(taken == std::vector<int>{42}); }
    }
  }
}

SCENARIO("Taken and leftover values are destroyed", "[mpsc_list]") {
  GIVEN("no tracked values alive") {
    REQUIRE(Tracked::live.load() == 0);

    WHEN("a batch of them is taken and dropped") {
      cutils::sync::MpscList<Tracked> list;
      list.Push(Tracked{1});
      list.Push(Tracked{2});
      {
        [[maybe_unused]] const auto batch = list.TakeAll();
      }

      THEN("none are alive") { CHECK(Tracked::live.load() == 0); }
    }

    WHEN("a list is destroyed with values never taken") {
      {
        cutils::sync::MpscList<Tracked> list;
        list.Push(Tracked{1});
        list.Push(Tracked{2});
      }

      THEN("none are alive") { CHECK(Tracked::live.load() == 0); }
    }
  }
}
