// See LICENSE in the repository root.

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using cutils::Idle;
using Spin = std::integral_constant<Idle, Idle::kSpin>;
using Park = std::integral_constant<Idle, Idle::kPark>;

constexpr std::size_t kUrgent = 0;
constexpr std::size_t kDemoted = 1;
constexpr std::size_t kTwoTiers = 2;

template <typename Task = int, typename Result = void, bool NoThrow = true>
struct WorkShape {
  using task_type = Task;

  template <typename Context>
  auto Process(Task, Context&) noexcept(NoThrow) -> Result;
};

static_assert(cutils::IsWorker<WorkShape<>>);
static_assert(!cutils::IsWorker<int>);
static_assert(!cutils::IsWorker<WorkShape<std::unique_ptr<int>>>);
static_assert(!cutils::IsWorker<WorkShape<int, bool>>);
static_assert(!cutils::IsWorker<WorkShape<int, void, false>>);

/// Sums a per-worker counter across every slot the scheduler exposes.
template <typename Scheduler, typename Field>
auto SumAcrossWorkers(Scheduler& scheduler, Field field) -> std::size_t {
  std::size_t total = 0;
  for (const auto& worker : scheduler.Workers()) {
    total += worker.*field;
  }
  return total;
}

/// Records a visit for every task and fans out into a complete binary tree.
struct TreeWork {
  using task_type = std::size_t;

  explicit TreeWork(std::span<std::atomic<unsigned>> seen) : seen_(seen) {}

  void Process(task_type task, auto& context) noexcept {
    seen_[task].fetch_add(1, std::memory_order_relaxed);
    ++count;
    for (const auto child : {2 * task + 1, 2 * task + 2}) {
      if (child < seen_.size()) {
        context.Submit(child);
      }
    }
  }

  std::span<std::atomic<unsigned>> seen_;
  std::size_t count{0};
};

struct Payload {
  mutable std::atomic<unsigned> visits{0};
};

/// Records a visit on the payload each task points at.
struct VisitWork {
  using task_type = const Payload*;

  void Process(task_type task, auto&) noexcept {
    task->visits.fetch_add(1, std::memory_order_relaxed);
  }
};

/// Walks a chain of tasks, each re-submitting its successor at a demoted
/// priority.
struct DemotedChain {
  using task_type = std::size_t;

  std::size_t count = 0;

  void Process(task_type depth, auto& context) noexcept {
    if (depth > 0) {
      context.Submit(depth - 1, kDemoted);
    }
    ++count;
  }
};

/// Fans out into a binary tree whose two children land on different
/// priorities.
struct MixedWork {
  using task_type = std::size_t;

  std::size_t count = 0;

  void Process(task_type depth, auto& context) noexcept {
    if (depth > 0) {
      context.Submit(depth - 1, kUrgent);
      context.Submit(depth - 1, kDemoted);
    }
    ++count;
  }
};

/// The root task submits a child into its own queue and then blocks until the
/// child has run, so the child can only make progress by being stolen.
struct StealingWork {
  using task_type = int;

  std::binary_semaphore* child_done;
  std::atomic<bool>* stolen;
  std::size_t priority;

  void Process(task_type task, auto& context) noexcept {
    if (task == 0) {
      context.Submit(1, priority);
      stolen->store(child_done->try_acquire_for(std::chrono::seconds(10)),
                    std::memory_order_relaxed);
    } else {
      child_done->release();
    }
  }
};

/// Takes a token from inside its task and parks it where the test can drop it.
struct HoldingWork {
  using task_type = int;

  std::optional<cutils::WorkToken>* held;

  /// @brief Set once `held` is filled, publishing it to the test thread.
  std::atomic<bool>* taken;

  void Process(task_type, auto& context) noexcept {
    held->emplace(context.Hold());
    taken->store(true, std::memory_order_release);
  }
};

/// Logs the order tasks are selected in; the root submits one task at each
/// priority, demoted first.
struct PriorityOrder {
  using task_type = int;

  std::array<int, 3> selected{};
  std::size_t count = 0;

  void Process(task_type task, auto& context) noexcept {
    selected[count++] = task;
    if (task == 0) {
      context.Submit(1, kDemoted);
      context.Submit(2, kUrgent);
    }
  }
};
}

SCENARIO("A scheduler asked for zero workers still has one",
         "[task_scheduler]") {
  GIVEN("a scheduler constructed with a worker count of zero") {
    cutils::TaskScheduler<VisitWork> executor(0);

    THEN("it exposes exactly one worker") {
      REQUIRE(executor.Workers().size() == 1);
    }

    WHEN("it is waited on without any submitted tasks") {
      executor.Wait();
      THEN("the wait returns") { SUCCEED(); }
    }
  }
}

SCENARIO("Every externally submitted task runs exactly once",
         "[task_scheduler]") {
  GIVEN("a four-worker scheduler and many independent payloads") {
    constexpr std::size_t kCount = 8192;
    std::vector<Payload> payloads(kCount);
    cutils::TaskScheduler<VisitWork> executor(4);

    WHEN("each payload is submitted from outside the scheduler") {
      for (const auto& payload : payloads) {
        REQUIRE(executor.Submit(&payload));
      }
      executor.Wait();

      THEN("each payload was visited exactly once") {
        REQUIRE(std::ranges::all_of(payloads, [](const Payload& payload) {
          return payload.visits.load(std::memory_order_relaxed) == 1;
        }));
      }
    }
  }
}

TEMPLATE_TEST_CASE(
    "Scenario: Tasks submitted from within workers run "
    "exactly once",
    "[task_scheduler]", Spin, Park) {
  const auto workers = GENERATE(0UZ, 1UZ, 4UZ, 8UZ);
  CAPTURE(workers);

  GIVEN("a scheduler whose tasks fan out into a complete binary tree") {
    constexpr std::size_t kCount = 8191;
    std::vector<std::atomic<unsigned>> seen(kCount);
    cutils::TaskScheduler<TreeWork, 1, TestType::value> executor(
        workers, TreeWork(seen));

    WHEN("the root is submitted and the scheduler is waited on") {
      REQUIRE(executor.Submit(0));
      executor.Wait();

      THEN("every node was visited exactly once") {
        REQUIRE(std::ranges::all_of(seen, [](const auto& visits) {
          return visits.load(std::memory_order_relaxed) == 1;
        }));

        AND_THEN("the workers' own state accounts for every node") {
          REQUIRE(SumAcrossWorkers(executor, &TreeWork::count) == kCount);
        }
      }
    }
  }
}

SCENARIO("Work submitted at a demoted priority is not starved",
         "[task_scheduler]") {
  const auto workers = GENERATE(1UZ, 2UZ, 4UZ);
  CAPTURE(workers);

  GIVEN("a two-tier scheduler running a chain of demoted tasks") {
    constexpr std::size_t kDepth = 500;
    cutils::TaskScheduler<DemotedChain, kTwoTiers> executor(workers);

    WHEN("the head of the chain is submitted and the scheduler is waited on") {
      REQUIRE(executor.Submit(kDepth));
      executor.Wait();

      THEN("every link of the chain was processed") {
        REQUIRE(SumAcrossWorkers(executor, &DemotedChain::count) == kDepth + 1);
      }
    }
  }
}

SCENARIO("Work split across priorities all completes", "[task_scheduler]") {
  GIVEN("a two-tier scheduler whose tasks fan out into both tiers") {
    constexpr std::size_t kDepth = 12;
    cutils::TaskScheduler<MixedWork, kTwoTiers> executor(4);

    WHEN("the root is submitted and the scheduler is waited on") {
      REQUIRE(executor.Submit(kDepth));
      executor.Wait();

      THEN("every node of the tree was processed") {
        REQUIRE(SumAcrossWorkers(executor, &MixedWork::count) ==
                (1UZ << (kDepth + 1)) - 1);
      }
    }
  }
}

SCENARIO("An idle worker steals from a busy worker's queue",
         "[task_scheduler]") {
  const auto priority = GENERATE(kUrgent, kDemoted);
  CAPTURE(priority);

  GIVEN("two workers and a root task that blocks until its child has run") {
    std::binary_semaphore child_done(0);
    std::atomic<bool> stolen{false};
    cutils::TaskScheduler<StealingWork, kTwoTiers> executor(
        2, StealingWork{&child_done, &stolen, priority});

    WHEN("the root is submitted and the scheduler is waited on") {
      REQUIRE(executor.Submit(0));
      executor.Wait();

      THEN("the child ran on the other worker while the root was blocked") {
        REQUIRE(stolen.load(std::memory_order_relaxed));
      }
    }
  }
}

SCENARIO("A worker prefers urgent work over demoted work", "[task_scheduler]") {
  GIVEN("a single-worker, two-tier scheduler") {
    cutils::TaskScheduler<PriorityOrder, kTwoTiers> executor(1);

    WHEN("a task enqueues demoted work before urgent work") {
      REQUIRE(executor.Submit(0));
      executor.Wait();

      THEN("the urgent task is selected before the demoted one") {
        const auto& worker = *executor.Workers().begin();
        REQUIRE(worker.count == 3);
        REQUIRE(worker.selected == std::array{0, 2, 1});
      }
    }
  }
}

namespace {

/// CPU time this process has used, user and system.
auto ProcessCpuTime() -> std::chrono::microseconds {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto total = [](const timeval& tv) {
    return std::chrono::seconds(tv.tv_sec) +
           std::chrono::microseconds(tv.tv_usec);
  };
  return total(usage.ru_utime) + total(usage.ru_stime);
}

/// Poll `done` until it holds, for at most two seconds.
template <class Predicate>
auto Eventually(Predicate done) -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// Long enough that idle workers have gone to sleep.
constexpr auto kSettle = std::chrono::milliseconds(50);

}  // namespace

TEMPLATE_TEST_CASE("Scenario: Wait does not return while a work token is held",
                   "[task_scheduler]", Spin, Park) {
  GIVEN("a scheduler with one token held and a thread waiting on it") {
    cutils::TaskScheduler<VisitWork, 1, TestType::value> executor(2);
    std::optional token{executor.Hold()};
    std::atomic<bool> returned{false};
    std::jthread waiter([&] {
      executor.Wait();
      returned.store(true);
    });

    WHEN("time passes with the token still held") {
      std::this_thread::sleep_for(kSettle);

      THEN("the wait has not returned") { CHECK_FALSE(returned.load()); }
    }

    WHEN("the token is dropped") {
      token.reset();
      waiter.join();

      THEN("the wait returns") { CHECK(returned.load()); }
    }

    // The waiter's join, on scope exit, needs the token gone.
    token.reset();
  }
}

SCENARIO("A parked worker wakes for work submitted later", "[task_scheduler]") {
  GIVEN("a one-worker parking scheduler that has gone idle") {
    std::vector<Payload> payloads(2);
    cutils::TaskScheduler<VisitWork, 1, Idle::kPark> executor(1);
    std::optional token{executor.Hold()};
    REQUIRE(executor.Submit(&payloads[0]));
    REQUIRE(Eventually([&] { return payloads[0].visits.load() == 1; }));
    std::this_thread::sleep_for(kSettle);

    WHEN("another task is submitted from outside") {
      REQUIRE(executor.Submit(&payloads[1]));

      THEN("the worker runs it") {
        CHECK(Eventually([&] { return payloads[1].visits.load() == 1; }));
      }
    }

    token.reset();
    executor.Wait();
  }
}

SCENARIO("Parked workers use no CPU while there is nothing to do",
         "[task_scheduler]") {
  GIVEN("four parking workers, all started and then left without work") {
    constexpr std::size_t kWorkers = 4;
    std::vector<Payload> payloads(kWorkers);
    cutils::TaskScheduler<VisitWork, 1, Idle::kPark> executor(kWorkers);
    std::optional token{executor.Hold()};
    for (auto& payload : payloads) {
      REQUIRE(executor.Submit(&payload));
    }
    REQUIRE(Eventually([&] {
      return std::ranges::all_of(payloads, [](const Payload& payload) {
        return payload.visits.load() == 1;
      });
    }));
    std::this_thread::sleep_for(kSettle);

    WHEN("they sit idle for 300 ms") {
      const auto before = ProcessCpuTime();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      const auto used = ProcessCpuTime() - before;

      THEN("the process used well under one core's worth of that time") {
        CAPTURE(used.count());
        CHECK(used < std::chrono::milliseconds(100));
      }
    }

    token.reset();
    executor.Wait();
  }
}

SCENARIO("A token taken inside a task keeps Wait from returning",
         "[task_scheduler]") {
  GIVEN("a task that leaves a token behind, and a thread waiting") {
    std::optional<cutils::WorkToken> held;
    std::atomic<bool> taken{false};
    cutils::TaskScheduler<HoldingWork, 1, Idle::kPark> executor(
        1, HoldingWork{&held, &taken});
    REQUIRE(executor.Submit(0));
    REQUIRE(Eventually([&] { return taken.load(std::memory_order_acquire); }));
    std::atomic<bool> returned{false};
    std::jthread waiter([&] {
      executor.Wait();
      returned.store(true);
    });
    std::this_thread::sleep_for(kSettle);

    THEN("the wait has not returned") { CHECK_FALSE(returned.load()); }

    WHEN("the token is dropped") {
      held.reset();
      waiter.join();

      THEN("the wait returns") { CHECK(returned.load()); }
    }

    // The waiter's join, on scope exit, needs the token gone.
    held.reset();
  }
}
