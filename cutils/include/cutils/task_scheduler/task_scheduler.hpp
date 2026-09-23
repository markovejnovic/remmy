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
#include <cutils/collections/heap_array.hpp>
#include <cutils/concepts.hpp>
#include <cutils/workstealing_queue/workstealing_queue.hpp>
#include <memory>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>
#ifndef NDEBUG
#include <cassert>
#include <utility>
#endif

namespace cutils {

namespace detail {

template <TriviallyCopyable Task>
struct TaskContextInterface {
  void Submit(const Task& task, std::size_t priority = 0) noexcept;
};

}  // namespace detail

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
  /// @note Thread safe.
  [[nodiscard]] auto Submit(const task_type& task,
                            std::size_t priority = 0) noexcept -> bool {
    if (activity_.stop_.load(std::memory_order_relaxed)) {
      return false;
    }
    activity_.pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    submitted_[priority].Insert(task);

    MaybeGrowThreads();
    return true;
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
    MaybeGrowThreads();
  }

  /// @brief Send a stop signal to all the threads and wait for them to finish.
  void StopAndJoinThreads() {
    activity_.stop_.store(true, std::memory_order_relaxed);
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
        std::this_thread::yield();
        continue;
      }

      self.worker.Process(*task, context);
      if (activity_.pending_tasks_.fetch_sub(1, std::memory_order_relaxed) ==
          1) {
        activity_.pending_tasks_.notify_one();
      }

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
};

}  // namespace cutils

#endif  // CUTILS_TASK_SCHEDULER_TASK_SCHEDULER_HPP
