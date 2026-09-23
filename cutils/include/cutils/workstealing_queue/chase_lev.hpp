// See LICENSE in the repository root.

#ifndef CUTILS_WORKSTEALING_QUEUE_CHASE_LEV_HPP
#define CUTILS_WORKSTEALING_QUEUE_CHASE_LEV_HPP

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cutils/concepts.hpp>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace cutils {

/// Work-stealing queue implemented by a a Chase-Lev strategy, improved by
/// Le-Pop-Coeh-Nardelli.
///
/// This is the bog-standard, simple work-stealing queue.
///
/// There are two important thread-classes:
///
///   - The *owner* thread is the thread that owns an instance of this queue.
///   - The *thief* thread is a thread which goes into other threads'
///     ChaseLev instances and steals values from them.
///
/// @tparam T - The type of the value stored in the queue.
/// @tparam Result - The type of the value returned by
///                  [`ChaseLev::Take`] and
///                  [`ChaseLev::Steal`]. By default, it is a
///                  [`std::optional<T>`], but you can override it to be
///                  whatever you like, enabling you to avoid the extra bool
///                  added in the [`std::optional`].
///
///                  The semantics of [`Result`] **must be**: default
///                  initialization means "empty" and there must be a
///                  constructor from `T` meaning "contains an instance of T".
template <TriviallyCopyable T, typename Result = std::optional<T>>
  requires std::default_initializable<Result> &&
           std::constructible_from<Result, T>
struct ChaseLev {
  using value_type = T;
  using result_type = Result;

  static constexpr std::size_t kInitCap = 1024;

  /// Create an empty Chase-Lev work-stealing queue.
  ChaseLev() : top_(0), bottom_(0) {
    auto d = std::make_unique<Data>();
    d->len_.store(kInitCap, std::memory_order_relaxed);
    d->buffer_ = std::make_unique<std::atomic<T>[]>(kInitCap);
    buffers_.push_back(std::move(d));
    data_.store(buffers_.back().get(), std::memory_order_relaxed);
  }

  /// Copying the work-stealing queue is not allowed as it is unsafe.
  ChaseLev(const ChaseLev&) = delete;

  /// Copying the work-stealing queue is not allowed as it is unsafe.
  auto operator=(const ChaseLev&) -> ChaseLev& = delete;

  /// Moving is allowed only before any worker touches the queue. Construction
  /// is single-threaded, so this is a plain transfer of the buffers and the
  /// index protocol; the moved-from queue is left empty and owns nothing.
  ChaseLev(ChaseLev&& other) noexcept
      : top_(other.top_.load(std::memory_order_relaxed)),
        bottom_(other.bottom_.load(std::memory_order_relaxed)),
        data_(other.data_.load(std::memory_order_relaxed)),
        buffers_(std::move(other.buffers_)) {
    other.data_.store(nullptr, std::memory_order_relaxed);
  }

  /// Grab an entry from the work-stealing queue.
  ///
  /// Only the **owner** can safely call this function.
  [[nodiscard]] auto Take() -> Result {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    const Data* a = data_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_relaxed);

    if (t <= b) {
      // Non-empty queue.
      const std::size_t len = a->len_.load(std::memory_order_relaxed);
      T val = a->buffer_[static_cast<std::size_t>(b) % len].load(
          std::memory_order_acquire);
      if (t == b) {
        // Single last element: race against concurrent steals for it.
        const bool claimed = top_.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        bottom_.store(b + 1, std::memory_order_relaxed);
        if (!claimed) {
          return Result{};  // Lost the race.
        }
      }
      return Result{std::move(val)};
    }
    // Empty queue.
    bottom_.store(b + 1, std::memory_order_relaxed);
    return Result{};
  }

  /// Insert an entry into the queue.
  ///
  /// Only the **owner** can safely call this function.
  void Insert(const T& val) {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_acquire);
    const Data* a = data_.load(std::memory_order_relaxed);

    if (b - t >
        static_cast<std::int64_t>(a->len_.load(std::memory_order_relaxed)) -
            1) {
      // The queue is full.
      Resize();
      a = data_.load(std::memory_order_relaxed);
    }

    const std::size_t len = a->len_.load(std::memory_order_relaxed);
    // Explicitly publish pointee contents too: TSan cannot model the fence
    // synchronization alone. Keep the original index protocol and its fences.
    a->buffer_[static_cast<std::size_t>(b) % len].store(
        std::move(val), std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  /// Steal an entry from this queue.
  ///
  /// Only the **thief** can safely call this function.
  [[nodiscard]] auto Steal() -> Result {
    std::int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = bottom_.load(std::memory_order_acquire);

    if (t < b) {
      // Non-empty queue.
      const Data* a =
          data_.load(std::memory_order_acquire);  // ref uses consume
      const std::size_t len = a->len_.load(std::memory_order_relaxed);
      T val = a->buffer_[static_cast<std::size_t>(t) % len].load(
          std::memory_order_acquire);
      if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
        return Result{};  // Lost the race (ABORT).
      }
      return Result{std::move(val)};
    }
    return Result{};  // Empty.
  }

 private:
  struct Data {
    /// Note that this atomic is necessary since it is touched by both the
    /// owner thread and the thief threads.
    std::atomic<std::size_t> len_;
    std::unique_ptr<std::atomic<T>[]> buffer_;
  };

  /// Double the size of the buffer.
  ///
  /// This can only be safely called by the owner thread.
  void Resize() {
    const Data* old = data_.load(std::memory_order_relaxed);
    const std::size_t old_len = old->len_.load(std::memory_order_relaxed);
    const std::size_t new_len = old_len * 2;

    auto nd = std::make_unique<Data>();
    nd->len_.store(new_len, std::memory_order_relaxed);
    nd->buffer_ = std::make_unique<std::atomic<T>[]>(new_len);

    const std::int64_t t = top_.load(std::memory_order_relaxed);
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    for (std::int64_t i = t; i < b; ++i) {
      const T value = old->buffer_[static_cast<std::size_t>(i) % old_len].load(
          std::memory_order_acquire);
      nd->buffer_[static_cast<std::size_t>(i) % new_len].store(
          value, std::memory_order_release);
    }

    buffers_.push_back(std::move(nd));
    data_.store(buffers_.back().get(), std::memory_order_release);
  }

  /// The top of the buffer.
  ///
  /// Must remain signed, since `Take()` can underflow bottom - 1.
  alignas(std::hardware_destructive_interference_size)
      std::atomic<std::int64_t> top_;

  /// Bottom of the buffer.
  ///
  /// Must remain signed, since `Take()` can underflow bottom - 1.
  alignas(std::hardware_destructive_interference_size)
      std::atomic<std::int64_t> bottom_;

  /// Each thread operates on this pointer to indicate where the active data
  /// currently is.
  ///
  /// This pointer must be atomic since multiple threads access it at the same
  /// time.
  alignas(std::hardware_destructive_interference_size) std::atomic<Data*> data_;

  /// Owns the active and retired buffers. Only the owner mutates this list.
  std::vector<std::unique_ptr<Data>> buffers_;
};

}  // namespace cutils

#endif  // CUTILS_WORKSTEALING_QUEUE_CHASE_LEV_HPP
