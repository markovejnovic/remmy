// See LICENSE in the repository root.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace {
using cutils::WorkStealingQueue;
using Queue = WorkStealingQueue<int>;
constexpr int kInitCap = static_cast<int>(Queue::kInitCap);

/// Result type whose default state means "empty" and which cannot be copied or
/// moved, so the queue must construct it in place exactly once per claim.
struct FdResult {
  inline static std::atomic<int> constructed{0};

  FdResult() = default;
  explicit FdResult(int&& fd) : value_(fd) {
    constructed.fetch_add(1, std::memory_order_relaxed);
  }
  FdResult(const FdResult&) = delete;
  FdResult(FdResult&&) = delete;
  auto operator=(const FdResult&) -> FdResult& = delete;
  auto operator=(FdResult&&) -> FdResult& = delete;

  [[nodiscard]] auto Value() const -> int { return value_; }

 private:
  int value_{-1};
};

struct Payload {
  int value;
  int checksum;
  std::string text;
};

struct NeedsValue {
  explicit NeedsValue(int);
};

template <typename T>
concept AcceptsValue = requires { typename WorkStealingQueue<T>; };
template <typename Result>
concept AcceptsResult = requires { typename WorkStealingQueue<int, Result>; };

static_assert(cutils::IsWorkStealingQueue<Queue>);
static_assert(cutils::IsWorkStealingQueue<WorkStealingQueue<int, FdResult>>);
static_assert(!AcceptsValue<std::unique_ptr<int>>);
static_assert(!AcceptsResult<NeedsValue>);
static_assert(!AcceptsResult<int*>);
}

SCENARIO("Polling an empty queue leaves it usable", "[chase_lev]") {
  GIVEN("a new queue") {
    Queue queue;
    WHEN("the owner takes and a thief steals") {
      const auto taken = queue.Take();
      const auto stolen = queue.Steal();
      THEN("both find it empty") {
        REQUIRE_FALSE(taken);
        REQUIRE_FALSE(stolen);
      }
      AND_WHEN("a value is inserted afterwards") {
        queue.Insert(7);
        THEN("the owner takes that value and the queue is empty again") {
          REQUIRE(queue.Take() == 7);
          REQUIRE_FALSE(queue.Take());
          REQUIRE_FALSE(queue.Steal());
        }
      }
    }
  }
}

SCENARIO("The owner takes newest first while thieves steal oldest first",
         "[chase_lev]") {
  GIVEN("a queue filled around its growth boundaries") {
    const int count =
        GENERATE(1, kInitCap - 1, kInitCap, kInitCap + 1, 8 * kInitCap + 1);
    CAPTURE(count);
    Queue queue;
    for (int i = 0; i < count; ++i) {
      queue.Insert(i);
    }
    WHEN("a thief steals the first half and the owner takes the rest") {
      bool fifo = true;
      for (int i = 0; i < count / 2; ++i) {
        fifo = fifo && queue.Steal() == i;
      }
      bool lifo = true;
      for (int i = count - 1; i >= count / 2; --i) {
        lifo = lifo && queue.Take() == i;
      }
      THEN("steals are FIFO, takes are LIFO, and nothing remains") {
        REQUIRE(fifo);
        REQUIRE(lifo);
        REQUIRE_FALSE(queue.Take());
        REQUIRE_FALSE(queue.Steal());
      }
    }
  }
}

SCENARIO("A custom result type reports emptiness by default construction",
         "[chase_lev]") {
  GIVEN("a queue whose result is a sentinel-encoded descriptor") {
    WorkStealingQueue<int, FdResult> queue;
    THEN("polling it empty yields the default sentinel") {
      REQUIRE(queue.Take().Value() == -1);
      REQUIRE(queue.Steal().Value() == -1);
    }
    WHEN("a value is inserted and taken by the owner") {
      queue.Insert(3);
      const auto taken = queue.Take();
      THEN("the owner receives it and a thief finds nothing") {
        REQUIRE(taken.Value() == 3);
        REQUIRE(queue.Steal().Value() == -1);
      }
    }
    WHEN("a value is inserted and stolen by a thief") {
      queue.Insert(4);
      const auto stolen = queue.Steal();
      THEN("the thief receives it and the owner finds nothing") {
        REQUIRE(stolen.Value() == 4);
        REQUIRE(queue.Take().Value() == -1);
      }
    }
  }
}

SCENARIO("Owner and thief racing for the last element never both win",
         "[chase_lev]") {
  GIVEN("an owner and a thief contending for a single element per round") {
    constexpr int kRounds = 2000;
    WorkStealingQueue<int, FdResult> queue;
    FdResult::constructed.store(0, std::memory_order_relaxed);
    std::binary_semaphore start(0);
    std::binary_semaphore finish(0);
    int stolen = -1;

    WHEN("each round the owner takes while the thief steals") {
      std::jthread thief([&] {
        for (int i = 0; i < kRounds; ++i) {
          start.acquire();
          stolen = queue.Steal().Value();
          finish.release();
        }
      });
      bool exclusive = true;
      for (int i = 0; i < kRounds; ++i) {
        queue.Insert(i);
        start.release();
        const auto taken = queue.Take();
        finish.acquire();
        exclusive = exclusive && ((taken.Value() == i && stolen == -1) ||
                                  (taken.Value() == -1 && stolen == i));
      }
      thief.join();
      THEN("exactly one side claims each element; the loser builds nothing") {
        REQUIRE(exclusive);
        REQUIRE(FdResult::constructed.load(std::memory_order_relaxed) ==
                kRounds);
        REQUIRE(queue.Take().Value() == -1);
        REQUIRE(queue.Steal().Value() == -1);
      }
    }
  }
}

SCENARIO("A thief keeps FIFO order while the owner grows the buffer",
         "[chase_lev]") {
  GIVEN("a queue already filled to its initial capacity") {
    constexpr int kCount = 32 * kInitCap;
    Queue queue;
    for (int i = 0; i < kInitCap; ++i) {
      queue.Insert(i);
    }
    WHEN("the owner keeps inserting while a thief drains concurrently") {
      bool ordered = true;
      std::jthread thief([&] {
        for (int expected = 0; expected < kCount;) {
          if (const auto value = queue.Steal()) {
            ordered = ordered && *value == expected;
            ++expected;
          } else {
            std::this_thread::yield();
          }
        }
      });
      for (int i = kInitCap; i < kCount; ++i) {
        queue.Insert(i);
      }
      thief.join();
      THEN("every value is stolen once, in insertion order") {
        REQUIRE(ordered);
        REQUIRE_FALSE(queue.Take());
        REQUIRE_FALSE(queue.Steal());
      }
    }
  }
}

SCENARIO("Stolen pointers publish their pointee across reused slots",
         "[chase_lev]") {
  GIVEN("a queue of pointers to heap payloads") {
    constexpr int kBurst = 32 * kInitCap;
    constexpr int kCount = 5 * kBurst;
    WorkStealingQueue<const Payload*> queue;
    std::vector<std::unique_ptr<Payload>> payloads;
    payloads.reserve(kCount);
    std::atomic<bool> start{false};
    std::atomic<int> consumed{0};

    WHEN("the owner grows the buffer, then throttles so slots wrap") {
      bool valid = true;
      std::jthread thief([&] {
        while (!start.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        for (int expected = 0; expected < kCount;) {
          if (const auto item = queue.Steal()) {
            const Payload& payload = **item;
            valid = valid && payload.value == expected;
            valid = valid && payload.checksum == (expected ^ 0x5a5a);
            valid = valid && payload.text == std::to_string(expected);
            consumed.store(++expected, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
        }
      });
      for (int i = 0; i < kCount; ++i) {
        if (i >= kBurst) {
          while (i - consumed.load(std::memory_order_relaxed) >= kInitCap / 2) {
            std::this_thread::yield();
          }
        }
        auto payload =
            std::make_unique<Payload>(i, i ^ 0x5a5a, std::to_string(i));
        queue.Insert(payload.get());
        payloads.push_back(std::move(payload));
        if (i == 2 * kInitCap) {
          start.store(true, std::memory_order_relaxed);
        }
      }
      thief.join();
      THEN("the thief observes every payload fully written, in order") {
        REQUIRE(valid);
        REQUIRE(consumed.load(std::memory_order_relaxed) == kCount);
        REQUIRE_FALSE(queue.Take());
        REQUIRE_FALSE(queue.Steal());
      }
    }
  }
}
