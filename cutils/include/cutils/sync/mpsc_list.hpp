// See LICENSE in the repository root.

/// @brief A multi-producer, single-consumer list whose consumer sleeps.
///
/// Producers push onto the head with a CAS; the consumer takes the whole list
/// with one exchange and reverses it, as the Linux kernel's llist does. So
/// there is no ABA (no single node is ever popped), and a producer stopped
/// mid-push cannot stall the consumer past it.
///
/// Nothing a producer does can suspend it: a push is a CAS loop and, only when
/// the consumer has gone to sleep, one non-blocking wake syscall. Each
/// producer's values come out in the order it pushed them.
#ifndef CUTILS_SYNC_MPSC_LIST_HPP
#define CUTILS_SYNC_MPSC_LIST_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <utility>

#include "cutils/os/wait_on_address.hpp"

namespace cutils::sync {

/// @brief Unbounded MPSC list: any thread may Push, one thread may TakeAll.
template <class T>
class MpscList {
  struct Node {
    Node* next;
    T value;
  };

 public:
  /// @brief The values one TakeAll took, oldest first; destroys them when
  ///        dropped.
  class Batch {
   public:
    class iterator {
     public:
      using value_type = T;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::forward_iterator_tag;

      iterator() noexcept = default;

      [[nodiscard]] auto operator*() const noexcept -> T& {
        return node_->value;
      }

      auto operator++() noexcept -> iterator& {
        node_ = node_->next;
        return *this;
      }

      auto operator++(int) noexcept -> iterator {
        const iterator old = *this;
        ++*this;
        return old;
      }

      [[nodiscard]] auto operator==(const iterator&) const noexcept
          -> bool = default;

     private:
      friend Batch;

      explicit iterator(Node* node) noexcept : node_(node) {}

      Node* node_ = nullptr;
    };

    Batch(const Batch&) = delete;
    auto operator=(const Batch&) -> Batch& = delete;
    Batch(Batch&& other) noexcept
        : head_(std::exchange(other.head_, nullptr)) {}
    auto operator=(Batch&&) -> Batch& = delete;

    ~Batch() {
      while (head_ != nullptr) {
        delete std::exchange(head_, head_->next);
      }
    }

    [[nodiscard]] auto begin() const noexcept -> iterator {
      return iterator{head_};
    }

    [[nodiscard]] auto end() const noexcept -> iterator { return iterator{}; }

   private:
    friend MpscList;

    explicit Batch(Node* head) noexcept : head_(head) {}

    Node* head_;
  };

  MpscList() noexcept = default;
  MpscList(const MpscList&) = delete;
  MpscList(MpscList&&) = delete;
  auto operator=(const MpscList&) -> MpscList& = delete;
  auto operator=(MpscList&&) -> MpscList& = delete;

  ~MpscList() {
    [[maybe_unused]] const Batch leftovers{
        head_.exchange(nullptr, std::memory_order_acquire)};
  }

  /// @brief Append `value`. Never blocks; safe from any thread.
  void Push(T value) noexcept {
    auto* node = new Node{.next = head_.load(std::memory_order_relaxed),
                          .value = std::move(value)};
    while (!head_.compare_exchange_weak(node->next, node,
                                        std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
    }

    // Pairs with TakeAll's store-then-load of the two words: either we see
    // the consumer parked, or it sees our node. The exchange changes the word
    // so a wake that lands before the consumer's sleep is not lost to the
    // kernel's value check.
    if (parked_.load(std::memory_order_seq_cst) != 0 &&
        parked_.exchange(0, std::memory_order_seq_cst) != 0) {
      os::WakeOne(parked_);
    }
  }

  /// @brief Take every value pushed so far, oldest first, sleeping until
  ///        there is at least one. Only one thread may call this.
  [[nodiscard]] auto TakeAll() noexcept -> Batch {
    while (true) {
      if (Node* taken = head_.exchange(nullptr, std::memory_order_acquire);
          taken != nullptr) {
        return Batch{Reversed(taken)};
      }

      parked_.store(1, std::memory_order_seq_cst);
      if (head_.load(std::memory_order_seq_cst) == nullptr) {
        os::WaitOnAddress(parked_, 1);
      }
      parked_.store(0, std::memory_order_relaxed);
    }
  }

 private:
  [[nodiscard]] static auto Reversed(Node* newest) noexcept -> Node* {
    Node* oldest = nullptr;
    while (newest != nullptr) {
      oldest = std::exchange(newest, std::exchange(newest->next, oldest));
    }
    return oldest;
  }

  alignas(std::hardware_destructive_interference_size) std::atomic<Node*> head_{
      nullptr};

  /// @brief 1 while the consumer is about to sleep or asleep in TakeAll.
  std::atomic<std::uint32_t> parked_{0};
};

}  // namespace cutils::sync

#endif  // CUTILS_SYNC_MPSC_LIST_HPP
