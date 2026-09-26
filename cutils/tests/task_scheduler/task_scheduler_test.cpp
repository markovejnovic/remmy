// See LICENSE in the repository root.

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cutils/task_scheduler/task_scheduler.hpp>
#include <memory>
#include <semaphore>
#include <span>
#include <vector>

namespace {
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

SCENARIO("Tasks submitted from within workers run exactly once",
         "[task_scheduler]") {
  const auto workers = GENERATE(0UZ, 1UZ, 4UZ, 8UZ);
  CAPTURE(workers);

  GIVEN("a scheduler whose tasks fan out into a complete binary tree") {
    constexpr std::size_t kCount = 8191;
    std::vector<std::atomic<unsigned>> seen(kCount);
    cutils::TaskScheduler executor(workers, TreeWork(seen));

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

SCENARIO("A drained scheduler has run everything and takes more work",
         "[task_scheduler]") {
  const auto workers = GENERATE(1UZ, 4UZ);
  CAPTURE(workers);

  GIVEN("a scheduler whose tasks fan out into a complete binary tree") {
    constexpr std::size_t kCount = 4095;
    std::vector<std::atomic<unsigned>> seen(kCount);
    cutils::TaskScheduler executor(workers, TreeWork(seen));
    const auto visited_once = [&seen] {
      return std::ranges::all_of(seen, [](const auto& visits) {
        return visits.load(std::memory_order_relaxed) == 1;
      });
    };

    WHEN("the root is submitted and the scheduler is drained") {
      REQUIRE(executor.Submit(0));
      executor.Drain();

      THEN("every node was already visited exactly once") {
        REQUIRE(visited_once());
      }

      AND_WHEN("the root is submitted again and the scheduler is waited on") {
        for (auto& visits : seen) {
          visits.store(0, std::memory_order_relaxed);
        }
        REQUIRE(executor.Submit(0));
        executor.Drain();
        const bool drained_again = visited_once();
        executor.Wait();

        THEN("the second tree ran too, before the second drain returned") {
          REQUIRE(drained_again);
          REQUIRE(SumAcrossWorkers(executor, &TreeWork::count) == 2 * kCount);
        }
      }
    }
  }
}
