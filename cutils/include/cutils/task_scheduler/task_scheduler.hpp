// See LICENSE in the repository root.

/// @brief Cooperative task scheduler.
///
/// This module defines the core [`TaskScheduler`] structure, which is a
/// co-operative scheduler designed to run arbitrary tasks.
///
/// There are two important terms:
///
///   - An [`IsWorker`] is defined as a thread-pinned worker thread. It holds
///   its
///     own mutable state and **persists** across tasks that are scheduled.
///   - An [`IsWorker::task_type`] is a task that is injected into each worker,
///     when the previous worker finishes operation on a previous task.
///
/// This scheduler holds tasks in a work-stealing queue and each worker first
/// exhausts its own tasks, and then, when it's out of tasks to execute, tries
/// to steal elements from other workers' queues.
///
/// For more information on the queue, see [`workstealing_queue.hpp`].
#ifndef CUTILS_TASK_SCHEDULER_TASK_SCHEDULER_HPP
#define CUTILS_TASK_SCHEDULER_TASK_SCHEDULER_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cutils/collections/heap_array.hpp>
#include <cutils/concepts.hpp>
#include <cutils/os/wait_on_address.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#ifndef NDEBUG
#include <cassert>
#endif

namespace cutils {

namespace detail {
struct TokenIssuer;
}  // namespace detail

/// @brief Outstanding work a TaskScheduler does not run itself; its Wait
///        does not return while one is alive.
///
/// Not tied to the scheduler's type, so the work it stands for (a request
/// to another thread, say) can carry one without naming its scheduler.
class [[nodiscard]] WorkToken {
 public:
  WorkToken(const WorkToken&) = delete;
  auto operator=(const WorkToken&) -> WorkToken& = delete;
  WorkToken(WorkToken&& other) noexcept
      : pending_(std::exchange(other.pending_, nullptr)) {}
  auto operator=(WorkToken&&) -> WorkToken& = delete;

  ~WorkToken() {
    if (pending_ != nullptr &&
        pending_->fetch_sub(1, std::memory_order_relaxed) == 1) {
      pending_->notify_one();
    }
  }

 private:
  friend struct detail::TokenIssuer;

  explicit WorkToken(std::atomic<std::size_t>& pending) noexcept
      : pending_(&pending) {}

  std::atomic<std::size_t>* pending_;
};

namespace detail {

/// @brief The one way to make a WorkToken: count it into `pending` first.
struct TokenIssuer {
  [[nodiscard]] static auto Issue(std::atomic<std::size_t>& pending) noexcept
      -> WorkToken {
    pending.fetch_add(1, std::memory_order_relaxed);
    return WorkToken{pending};
  }
};

template <TriviallyCopyable Task>
struct TaskContextInterface {
  void Submit(const Task& task, std::size_t priority = 0) noexcept;
  [[nodiscard]] auto Hold() noexcept -> WorkToken;
};

}  // namespace detail

/// @brief What a worker does when it finds no task.
enum class Idle : std::uint8_t {
  /// Yield and look again. Lowest wake-up latency, but an idle worker keeps
  /// a core busy.
  kSpin,
  /// Sleep until work is submitted. Idle workers cost no CPU; every submit
  /// pays a fence and a load to see whether one needs waking.
  kPark,
};

/// @brief Persistent worker responsible for accepting and processing tasks.
template <typename Worker>
concept IsWorker =
    TriviallyCopyable<typename Worker::task_type> &&
    requires(Worker& worker, typename Worker::task_type task,
             detail::TaskContextInterface<typename Worker::task_type>& ctx) {
      { worker.Process(task, ctx) } noexcept -> std::same_as<void>;
    };

/// @brief Scheduler context injected into into each worker's
///        [`IsWorker::Process`] function.
///
/// This context enables you to submit more tasks into the scheduler.
template <typename Scheduler>
class TaskContext {
 public:
  using task_type = typename Scheduler::task_type;

  TaskContext(const TaskContext&) = delete;
  auto operator=(const TaskContext&) -> TaskContext& = delete;

  /// @brief Submit a new task into the scheduler, potentially increasing the
  ///        total number of threads in the scheduler.
  ///
  /// @note Thread-safe.
  void Submit(const task_type& task, std::size_t priority = 0) noexcept {
    scheduler_->Submit(*slot_, task, priority);
  }

  /// @brief See TaskScheduler::Hold.
  [[nodiscard]] auto Hold() noexcept -> WorkToken { return scheduler_->Hold(); }

 private:
  friend Scheduler;

  TaskContext(Scheduler& scheduler, typename Scheduler::Slot& slot) noexcept
      : scheduler_(&scheduler), slot_(&slot) {}

  Scheduler* scheduler_;
  typename Scheduler::Slot* slot_;
};

/// @brief A cooperative, lock-free scheduler for processing arbitrary tasks.
///
/// @todo Write some docs
template <IsWorker Worker, std::size_t kPriorities = 1,
          Idle kIdle = Idle::kSpin,
          typename Allocator = std::allocator<std::byte>>
class TaskScheduler {
  static_assert(kPriorities >= 1, "a scheduler needs at least one priority");

 public:
  using task_type = typename Worker::task_type;
  using allocator_type = Allocator;
  using ContextType = TaskContext<TaskScheduler>;

  static constexpr std::size_t priority_count = kPriorities;

  /// @brief Construct the TaskScheduler with a maximum number of workers.
  ///
  /// @groupstart
  ///
  /// @param worker_count The maximum number of threads this will ever spawn.
  ///                     Clamped to a minimum of 1.
  ///
  /// @param prototype A worker structure which gets copy-constructed into each
  ///                  new thread.
  ///
  /// @param allocator An optional allocator to use for allocating internal
  ///                  memory for the workers.
  explicit TaskScheduler(std::size_t worker_count,
                         const Allocator& allocator = Allocator{})
    requires std::default_initializable<Worker>
      : max_workers_(std::max<std::size_t>(1, worker_count)),
        slots_(max_workers_, SlotAllocator{allocator}) {}

  TaskScheduler(std::size_t worker_count, const Worker& prototype,
                const Allocator& allocator = Allocator{})
    requires std::copy_constructible<Worker>
      : max_workers_(std::max<std::size_t>(1, worker_count)),
        slots_(std::from_range,
               std::views::iota(std::size_t{0}, max_workers_) |
                   std::views::transform(
                       [&prototype](std::size_t) { return Slot(prototype); }),
               SlotAllocator{allocator}) {}
  /// @groupend

  TaskScheduler(const TaskScheduler&) = delete;
  auto operator=(const TaskScheduler&) -> TaskScheduler& = delete;

  ~TaskScheduler() { StopAndJoinThreads(); }

  /// @brief Submit a new task into the scheduler, potentially creating a new
  ///        thread if the number of threads is lower than max.
  ///
  /// @warning Only one thread outside the workers may submit, and only one at
  ///          a time: these land in a Chase-Lev deque whose push is owner-only.
  ///          Workers submit through their TaskContext instead.
  [[nodiscard]] auto Submit(const task_type& task,
                            std::size_t priority = 0) noexcept -> bool {
    if (activity_.stop_.load(std::memory_order_relaxed)) {
      return false;
    }
    activity_.pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    submitted_[priority].Insert(task);

    WakeParked();
    MaybeGrowThreads();
    return true;
  }

  /// @brief Count work that happens outside the scheduler, such as a task
  ///        waiting on another thread before it can be submitted, so Wait
  ///        does not finish under it. Thread safe.
  [[nodiscard]] auto Hold() noexcept -> WorkToken {
    return detail::TokenIssuer::Issue(activity_.pending_tasks_);
  }

  /// @brief Wait for the scheduler to finish its tasks.
  ///
  /// @note After calling `Wait`, the scheduler cannot be re-used. All
  ///       subsequent activity on the scheduler is considered UB.
  void Wait() noexcept {
#ifndef NDEBUG
    assert(!std::exchange(waited_, true) &&
           "TaskScheduler::Wait invoked multiple times");
#endif

    for (auto owed = activity_.pending_tasks_.load(std::memory_order_relaxed);
         owed != 0;
         owed = activity_.pending_tasks_.load(std::memory_order_relaxed)) {
      activity_.pending_tasks_.wait(owed, std::memory_order_relaxed);
    }

    StopAndJoinThreads();
  }

  /// @brief Borrow references for each of the active workers.
  ///
  /// @warn There are no thread-safety guarantees -- if you do not have a
  ///       synchronization system in the worker, access is inherently racy.
  [[nodiscard]] auto Workers(this auto& self) {
    return self.slots_ | std::views::transform(
                             [](auto& slot) -> auto& { return slot.worker; });
  }

 private:
  friend ContextType;

  /// @brief The single slot a worker is coupled to.
  struct Slot {
    std::array<WorkStealingQueue<task_type>, kPriorities> queues;

    /// @brief Persistent worker state injected by the caller.
    Worker worker;

    /// @brief Handle to the thread of the worker.
    std::jthread thread;

    Slot()
      requires std::default_initializable<Worker>
        : worker() {}
    explicit Slot(const Worker& prototype) : worker(prototype) {}
  };

  using SlotAllocator =
      typename std::allocator_traits<Allocator>::template rebind_alloc<Slot>;

  /// @brief Submit a task into a worker's own slot.
  void Submit(Slot& slot, const task_type& task,
              std::size_t priority) noexcept {
    activity_.pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    slot.queues[priority].Insert(task);
    WakeParked();
    MaybeGrowThreads();
  }

  /// @brief Mark one task or token done, waking Wait on the last one.
  void FinishOne() noexcept {
    if (activity_.pending_tasks_.fetch_sub(1, std::memory_order_relaxed) == 1) {
      activity_.pending_tasks_.notify_one();
    }
  }

  /// @brief Wake one parked worker, if any, for a task just inserted.
  void WakeParked() noexcept {
    if constexpr (kIdle == Idle::kPark) {
      // Pairs with the fence in ParkUntilWork: either we see the worker
      // counted as parked, or it sees the task we inserted.
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (parking_.parked.load(std::memory_order_relaxed) != 0) {
        // Changing the word is what makes a wake that beats the worker's
        // sleep count: its WaitOnAddress then sees a stale epoch.
        parking_.epoch.fetch_add(1, std::memory_order_relaxed);
        os::WakeOne(parking_.epoch);
      }
    }
  }

  /// @brief Sleep until a task may be available, taking it if one already is.
  [[nodiscard]] auto ParkUntilWork(Slot& self) noexcept
      -> std::tuple<std::optional<task_type>, std::size_t> {
    const std::uint32_t seen = parking_.epoch.load(std::memory_order_seq_cst);
    parking_.parked.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    auto acquired = AcquireNextTask(self);
    if (!std::get<0>(acquired) &&
        !activity_.stop_.load(std::memory_order_relaxed)) {
      os::WaitOnAddress(parking_.epoch, seen);
    }
    parking_.parked.fetch_sub(1, std::memory_order_relaxed);
    return acquired;
  }

  /// @brief Send a stop signal to all the threads and wait for them to finish.
  void StopAndJoinThreads() {
    activity_.stop_.store(true, std::memory_order_relaxed);
    if constexpr (kIdle == Idle::kPark) {
      // seq_cst so a worker that reads the new epoch also sees stop_.
      parking_.epoch.fetch_add(1, std::memory_order_seq_cst);
      os::WakeAll(parking_.epoch);
    }
    for (auto& slot : slots_) {
      if (slot.thread.joinable()) {
        slot.thread.join();
      }
    }
  }

  /// @brief Increase the total thread count.
  void MaybeGrowThreads() noexcept {
    for (auto index = activity_.live_threads_.load(std::memory_order_relaxed);
         index < max_workers_ &&
         activity_.pending_tasks_.load(std::memory_order_relaxed) != 0;) {
      if (activity_.live_threads_.compare_exchange_weak(
              index, index + 1, std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        slots_[index].thread =
            std::jthread([this, index] { WorkerLoop(slots_[index]); });
        return;
      }
    }
  }

  /// @brief Take the next enqueued task from the queues.
  ///
  /// @param priority The priority to pick from.
  [[nodiscard]] auto NextTask(std::size_t priority, Slot& self) noexcept
      -> std::optional<task_type> {
    auto task = self.queues[priority].Take();
    for (Slot& victim : slots_) {
      if (task) {
        break;
      }
      if (&victim != &self) {
        task = victim.queues[priority].Steal();
      }
    }
    return task;
  }

  /// @brief Try to take a task that has been explicitly submitted to the
  ///        scheduler.
  [[nodiscard]] auto TakeSubmitted(std::size_t priority) noexcept
      -> std::optional<task_type> {
    return submitted_[priority].Steal();
  }

  /// @brief Acquire the most urgent runnable task and the priority it came
  /// from.
  ///
  /// @return A tuple of the task (empty if none was found) and its priority
  ///         (`kPriorities` when empty).
  [[nodiscard]] auto AcquireNextTask(Slot& self) noexcept
      -> std::tuple<std::optional<task_type>, std::size_t> {
    for (std::size_t priority = 0; priority < kPriorities; ++priority) {
      auto task = NextTask(priority, self);
      if (!task) {
        task = TakeSubmitted(priority);
      }
      if (task) {
        return {std::move(task), priority};
      }
    }
    return {std::nullopt, kPriorities};
  }

  /// @brief The core loop that a worker executes.
  void WorkerLoop(Slot& self) noexcept {
    ContextType context(*this, self);

    while (true) {
      if (activity_.stop_.load(std::memory_order_relaxed)) {
        return;
      }

      [[maybe_unused]] auto [task, taken_priority] = AcquireNextTask(self);

      if (!task) {
        if constexpr (kIdle == Idle::kSpin) {
          std::this_thread::yield();
          continue;
        } else {
          std::tie(task, taken_priority) = ParkUntilWork(self);
          if (!task) {
            continue;
          }
        }
      }

      self.worker.Process(*task, context);
      FinishOne();

      if constexpr (kPriorities > 1) {
        if (taken_priority == kPriorities - 1) {
          std::this_thread::yield();
        }
      }
    }
  }

  alignas(std::hardware_destructive_interference_size) struct {
    /// @brief How many tasks are currently submitted to the scheduler.
    std::atomic<std::size_t> pending_tasks_{0};

    /// @brief Flag which finishes each thread. When we are done with threads,
    ///        we set this flag to true and busyloop for the threads to finish.
    std::atomic<bool> stop_{false};

    /// @brief The total number of threads that are currently alive.
    std::atomic<std::size_t> live_threads_{0};
  } activity_;

  const std::size_t max_workers_;

  HeapArray<Slot, SlotAllocator> slots_;

#ifndef NDEBUG
  bool waited_{false};
#endif

  /// @brief Holds each of the priorities of the scheduler.
  std::array<WorkStealingQueue<task_type>, kPriorities> submitted_;

  /// @brief Where Idle::kPark workers sleep; untouched under Idle::kSpin.
  alignas(std::hardware_destructive_interference_size) struct {
    /// @brief Bumped to wake parked workers; they sleep on its old value.
    std::atomic<std::uint32_t> epoch{0};

    /// @brief How many workers are parked or about to park.
    std::atomic<std::uint32_t> parked{0};
  } parking_;
};

}  // namespace cutils

#endif  // CUTILS_TASK_SCHEDULER_TASK_SCHEDULER_HPP
