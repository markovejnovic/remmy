// See LICENSE in the repository root.

#ifndef CUTILS_WORKSTEALING_QUEUE_WORKSTEALING_QUEUE_HPP
#define CUTILS_WORKSTEALING_QUEUE_WORKSTEALING_QUEUE_HPP

#include <concepts>
#include <cutils/concepts.hpp>
#include <cutils/workstealing_queue/chase_lev.hpp>
#include <optional>

namespace cutils {

/// The general API of a work-stealing queue.
template <typename Q>
concept IsWorkStealingQueue =
    requires(Q& queue, const typename Q::value_type& value) {
      { queue.Insert(value) } -> std::same_as<void>;
      { queue.Take() } -> std::same_as<typename Q::result_type>;
      { queue.Steal() } -> std::same_as<typename Q::result_type>;
    };

template <TriviallyCopyable T, typename Result = std::optional<T>>
using WorkStealingQueue = ChaseLev<T, Result>;

}  // namespace cutils

#endif  // CUTILS_WORKSTEALING_QUEUE_WORKSTEALING_QUEUE_HPP
