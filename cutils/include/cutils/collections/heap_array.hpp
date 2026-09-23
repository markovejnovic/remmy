// See LICENSE in the repository root.

/// @brief HeapArray is a non-resizable vec.
///
/// @warn This utility has been AI generated. I've surface-level reviewed it,
///       looks good. No guarantees.
#ifndef CUTILS_COLLECTIONS_HEAP_ARRAY_HPP
#define CUTILS_COLLECTIONS_HEAP_ARRAY_HPP

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cutils/concepts.hpp>
#include <cutils/exceptions/exceptions.hpp>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace cutils {

namespace detail {

struct SynthThreeWay {
  template <typename U, typename V>
    requires std::three_way_comparable_with<U, V>
  [[nodiscard]] constexpr auto operator()(const U& a, const V& b) const {
    return a <=> b;
  }

  template <typename U, typename V>
    requires(!std::three_way_comparable_with<U, V>) &&
            requires(const U& a, const V& b) {
              { a < b } -> std::convertible_to<bool>;
              { b < a } -> std::convertible_to<bool>;
            }
  [[nodiscard]] constexpr auto operator()(const U& a, const V& b) const
      -> std::weak_ordering {
    if (a < b) {
      return std::weak_ordering::less;
    }
    if (b < a) {
      return std::weak_ordering::greater;
    }
    return std::weak_ordering::equivalent;
  }
};

template <typename U, typename V = U>
using SynthThreeWayResult = decltype(SynthThreeWay{}(std::declval<const U&>(),
                                                     std::declval<const V&>()));

}  // namespace detail

/// @brief Contiguous iterator over a [`cutils::HeapArray`]'s storage.
template <typename T>
class HeapArrayIterator {
 public:
  using iterator_concept = std::contiguous_iterator_tag;
  using iterator_category = std::random_access_iterator_tag;
  using value_type = std::remove_cv_t<T>;
  using element_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = T*;
  using reference = T&;

  constexpr HeapArrayIterator() noexcept = default;

  explicit constexpr HeapArrayIterator(T* p) noexcept : ptr_(p) {}

  template <typename U>
    requires std::same_as<U, value_type> && std::is_const_v<T>
  constexpr HeapArrayIterator(  // NOLINT(google-explicit-constructor)
      const HeapArrayIterator<U>& o) noexcept
      : ptr_(o.ptr_) {}

  [[nodiscard]] constexpr auto operator*() const noexcept -> reference {
    return *ptr_;
  }

  [[nodiscard]] constexpr auto operator->() const noexcept -> pointer {
    return ptr_;
  }

  [[nodiscard]] constexpr auto operator[](difference_type n) const noexcept
      -> reference {
    return ptr_[n];
  }

  constexpr auto operator++() noexcept -> HeapArrayIterator& {
    ++ptr_;
    return *this;
  }

  constexpr auto operator++(int) noexcept -> HeapArrayIterator {
    HeapArrayIterator tmp{*this};
    ++ptr_;
    return tmp;
  }

  constexpr auto operator--() noexcept -> HeapArrayIterator& {
    --ptr_;
    return *this;
  }

  constexpr auto operator--(int) noexcept -> HeapArrayIterator {
    HeapArrayIterator tmp{*this};
    --ptr_;
    return tmp;
  }

  constexpr auto operator+=(difference_type n) noexcept -> HeapArrayIterator& {
    ptr_ += n;
    return *this;
  }

  constexpr auto operator-=(difference_type n) noexcept -> HeapArrayIterator& {
    ptr_ -= n;
    return *this;
  }

  [[nodiscard]] friend constexpr auto operator+(HeapArrayIterator it,
                                                difference_type n) noexcept
      -> HeapArrayIterator {
    it += n;
    return it;
  }

  [[nodiscard]] friend constexpr auto operator+(difference_type n,
                                                HeapArrayIterator it) noexcept
      -> HeapArrayIterator {
    it += n;
    return it;
  }

  [[nodiscard]] friend constexpr auto operator-(HeapArrayIterator it,
                                                difference_type n) noexcept
      -> HeapArrayIterator {
    it -= n;
    return it;
  }

  [[nodiscard]] constexpr auto operator-(
      const HeapArrayIterator& o) const noexcept -> difference_type {
    return ptr_ - o.ptr_;
  }

  [[nodiscard]] constexpr auto operator==(
      const HeapArrayIterator&) const noexcept -> bool = default;

  [[nodiscard]] constexpr auto operator<=>(
      const HeapArrayIterator&) const noexcept = default;

  template <typename U>
    requires(!std::same_as<U, T>) &&
            std::same_as<std::remove_const_t<U>, std::remove_const_t<T>>
  [[nodiscard]] friend constexpr auto operator==(
      const HeapArrayIterator& a, const HeapArrayIterator<U>& b) noexcept
      -> bool {
    return a.ptr_ == b.ptr_;
  }

  template <typename U>
    requires(!std::same_as<U, T>) &&
            std::same_as<std::remove_const_t<U>, std::remove_const_t<T>>
  [[nodiscard]] friend constexpr auto operator<=>(
      const HeapArrayIterator& a, const HeapArrayIterator<U>& b) noexcept
      -> std::strong_ordering {
    return a.ptr_ <=> b.ptr_;
  }

  template <typename U>
    requires(!std::same_as<U, T>) &&
            std::same_as<std::remove_const_t<U>, std::remove_const_t<T>>
  [[nodiscard]] friend constexpr auto operator-(
      const HeapArrayIterator& a, const HeapArrayIterator<U>& b) noexcept
      -> difference_type {
    return a.ptr_ - b.ptr_;
  }

 private:
  template <typename U>
  friend class HeapArrayIterator;

  T* ptr_{nullptr};
};

/// @brief A fixed-size, heap-allocated, allocator-aware dynamic array.
///
/// @tparam T Element type (a non-`const` object type).
/// @tparam Allocator Allocator whose `value_type` is `T`.
template <typename T, typename Allocator = std::allocator<T>>
class HeapArray {
  using alloc_traits = std::allocator_traits<Allocator>;
  static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                "HeapArray<T>: T must be a non-const object type");
  static_assert(std::same_as<T, typename Allocator::value_type>,
                "HeapArray<T, A>: A::value_type must be T");

 public:
  using value_type = T;
  using allocator_type = Allocator;
  using size_type = typename alloc_traits::size_type;
  using difference_type = typename alloc_traits::difference_type;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = typename alloc_traits::pointer;
  using const_pointer = typename alloc_traits::const_pointer;
  using iterator = HeapArrayIterator<T>;
  using const_iterator = HeapArrayIterator<const T>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// @brief Construct an empty array with a default-constructed allocator.
  constexpr HeapArray() noexcept(
      std::is_nothrow_default_constructible_v<Allocator>)
    requires std::default_initializable<Allocator>
  = default;

  /// @brief Construct an empty array with the given allocator.
  explicit constexpr HeapArray(const Allocator& alloc) noexcept
      : alloc_(alloc) {}

  /// @brief Construct `count` value-initialised elements.
  explicit constexpr HeapArray(size_type count,
                               const Allocator& alloc = Allocator())
    requires std::default_initializable<T>
      : alloc_(alloc) {
    data_ = BuildBlock(alloc_, count, [](Allocator& a, T* loc, size_type) {
      alloc_traits::construct(a, loc);
    });
    size_ = count;
  }

  /// @brief Construct `count` copies of `value`.
  constexpr HeapArray(size_type count, const T& value,
                      const Allocator& alloc = Allocator())
    requires std::copy_constructible<T>
      : alloc_(alloc) {
    data_ =
        BuildBlock(alloc_, count, [&value](Allocator& a, T* loc, size_type) {
          alloc_traits::construct(a, loc, value);
        });
    size_ = count;
  }

  /// @brief Construct from an iterator/sentinel range.
  template <std::forward_iterator It, std::sentinel_for<It> S>
    requires std::constructible_from<T, std::iter_reference_t<It>>
  constexpr HeapArray(It first, S last, const Allocator& alloc = Allocator())
      : alloc_(alloc) {
    const auto n = static_cast<size_type>(std::ranges::distance(first, last));
    data_ = BuildBlock(alloc_, n,
                       [it = first](Allocator& a, T* loc, size_type) mutable {
                         alloc_traits::construct(a, loc, *it);
                         ++it;
                       });
    size_ = n;
  }

  /// @brief Construct from a range (tag-dispatched).
  template <std::ranges::input_range R>
    requires(std::ranges::forward_range<R> || std::ranges::sized_range<R>) &&
            std::constructible_from<T, std::ranges::range_reference_t<R>>
  constexpr HeapArray(std::from_range_t, R&& range,
                      const Allocator& alloc = Allocator())
      : alloc_(alloc) {
    size_type n{};
    if constexpr (std::ranges::sized_range<R>) {
      n = static_cast<size_type>(std::ranges::size(range));
    } else {
      n = static_cast<size_type>(std::ranges::distance(range));
    }
    data_ = BuildBlock(alloc_, n,
                       [it = std::ranges::begin(range)](Allocator& a, T* loc,
                                                        size_type) mutable {
                         alloc_traits::construct(a, loc, *it);
                         ++it;
                       });
    size_ = n;
  }

  /// @brief Construct from an initialiser list.
  constexpr HeapArray(std::initializer_list<T> init,
                      const Allocator& alloc = Allocator())
    requires std::copy_constructible<T>
      : alloc_(alloc) {
    data_ = BuildBlock(
        alloc_, init.size(),
        [it = init.begin()](Allocator& a, T* loc, size_type) mutable {
          alloc_traits::construct(a, loc, *it);
          ++it;
        });
    size_ = init.size();
  }

  /// @brief Deep-copy constructor.
  constexpr HeapArray(const HeapArray& other)
    requires std::copy_constructible<T>
      : alloc_(
            alloc_traits::select_on_container_copy_construction(other.alloc_)) {
    data_ =
        BuildBlock(alloc_, other.size_,
                   [src = other.data_](Allocator& a, T* loc, size_type i) {
                     alloc_traits::construct(a, loc, std::to_address(src)[i]);
                   });
    size_ = other.size_;
  }

  /// @brief Allocator-extended deep-copy constructor.
  constexpr HeapArray(const HeapArray& other, const Allocator& alloc)
    requires std::copy_constructible<T>
      : alloc_(alloc) {
    data_ =
        BuildBlock(alloc_, other.size_,
                   [src = other.data_](Allocator& a, T* loc, size_type i) {
                     alloc_traits::construct(a, loc, std::to_address(src)[i]);
                   });
    size_ = other.size_;
  }

  /// @brief Move constructor (steals storage).
  constexpr HeapArray(HeapArray&& other) noexcept
      : data_(std::exchange(other.data_, pointer{})),
        size_(std::exchange(other.size_, size_type{0})),
        alloc_(std::move(other.alloc_)) {}

  /// @brief Allocator-extended move constructor.
  constexpr HeapArray(HeapArray&& other, const Allocator& alloc) noexcept(
      alloc_traits::is_always_equal::value)
      : alloc_(alloc) {
    if (alloc_ == other.alloc_) {
      data_ = std::exchange(other.data_, pointer{});
      size_ = std::exchange(other.size_, size_type{0});
    } else {
      data_ = BuildBlock(
          alloc_, other.size_,
          [src = other.data_](Allocator& a, T* loc, size_type i) {
            alloc_traits::construct(a, loc, std::move(std::to_address(src)[i]));
          });
      size_ = other.size_;
    }
  }

  /// @brief Destroy every element and release the storage.
  constexpr ~HeapArray() { DestroyAndDeallocate(); }

  /// @brief Deep-copy assignment (build-before-release).
  constexpr auto operator=(const HeapArray& other) -> HeapArray&
    requires std::copy_constructible<T>
  {
    if (this == &other) {
      return *this;
    }
    const auto copy_fn = [src = other.data_](Allocator& a, T* loc,
                                             size_type i) {
      alloc_traits::construct(a, loc, std::to_address(src)[i]);
    };
    constexpr bool kPocca =
        alloc_traits::propagate_on_container_copy_assignment::value;
    if constexpr (kPocca) {
      if (alloc_ != other.alloc_) {
        Allocator new_alloc = other.alloc_;
        pointer p = BuildBlock(new_alloc, other.size_, copy_fn);
        DestroyAndDeallocate();
        data_ = p;
        size_ = other.size_;
        alloc_ = other.alloc_;
        return *this;
      }
    }
    pointer p = BuildBlock(alloc_, other.size_, copy_fn);
    DestroyAndDeallocate();
    data_ = p;
    size_ = other.size_;
    if constexpr (kPocca) {
      alloc_ = other.alloc_;
    }
    return *this;
  }

  /// @brief Move assignment.
  constexpr auto operator=(HeapArray&& other) noexcept(
      alloc_traits::propagate_on_container_move_assignment::value ||
      alloc_traits::is_always_equal::value) -> HeapArray& {
    if (this == &other) {
      return *this;
    }
    constexpr bool kPocma =
        alloc_traits::propagate_on_container_move_assignment::value;
    if constexpr (kPocma) {
      DestroyAndDeallocate();
      alloc_ = std::move(other.alloc_);
      data_ = std::exchange(other.data_, pointer{});
      size_ = std::exchange(other.size_, size_type{0});
    } else {
      if (alloc_ == other.alloc_) {
        DestroyAndDeallocate();
        data_ = std::exchange(other.data_, pointer{});
        size_ = std::exchange(other.size_, size_type{0});
      } else {
        pointer p =
            BuildBlock(alloc_, other.size_,
                       [src = other.data_](Allocator& a, T* loc, size_type i) {
                         alloc_traits::construct(
                             a, loc, std::move(std::to_address(src)[i]));
                       });
        DestroyAndDeallocate();
        data_ = p;
        size_ = other.size_;
        // Leave `other` empty, matching every other move path.
        other.DestroyAndDeallocate();
        other.data_ = pointer{};
        other.size_ = size_type{0};
      }
    }
    return *this;
  }

  /// @brief Initialiser-list assignment (build-before-release).
  constexpr auto operator=(std::initializer_list<T> init) -> HeapArray&
    requires std::copy_constructible<T>
  {
    pointer p = BuildBlock(
        alloc_, init.size(),
        [it = init.begin()](Allocator& a, T* loc, size_type) mutable {
          alloc_traits::construct(a, loc, *it);
          ++it;
        });
    DestroyAndDeallocate();
    data_ = p;
    size_ = init.size();
    return *this;
  }

  /// @brief Obtain a copy of the allocator.
  [[nodiscard]] constexpr auto get_allocator() const noexcept
      -> allocator_type {
    return alloc_;
  }

  /// @brief Bounds-checked element access.
  [[nodiscard]] constexpr auto at(size_type pos) -> reference {
    if (pos >= size_) {
      ThrowOrAbort<std::out_of_range>("HeapArray::at: index out of range");
    }
    return data()[pos];
  }

  /// @brief Bounds-checked const element access.
  [[nodiscard]] constexpr auto at(size_type pos) const -> const_reference {
    if (pos >= size_) {
      ThrowOrAbort<std::out_of_range>("HeapArray::at: index out of range");
    }
    return data()[pos];
  }

  /// @brief Unchecked element access.
  [[nodiscard]] constexpr auto operator[](size_type pos) noexcept -> reference {
    return data()[pos];
  }

  /// @brief Unchecked const element access.
  [[nodiscard]] constexpr auto operator[](size_type pos) const noexcept
      -> const_reference {
    return data()[pos];
  }

  /// @brief Access the first element (precondition: `!empty()`).
  [[nodiscard]] constexpr auto front() noexcept -> reference {
    return data()[0];
  }

  /// @brief Access the first element, const (precondition: `!empty()`).
  [[nodiscard]] constexpr auto front() const noexcept -> const_reference {
    return data()[0];
  }

  /// @brief Access the last element (precondition: `!empty()`).
  [[nodiscard]] constexpr auto back() noexcept -> reference {
    return data()[size_ - 1];
  }

  /// @brief Access the last element, const (precondition: `!empty()`).
  [[nodiscard]] constexpr auto back() const noexcept -> const_reference {
    return data()[size_ - 1];
  }

  /// @brief Raw pointer to the storage.
  [[nodiscard]] constexpr auto data() noexcept -> value_type* {
    return std::to_address(data_);
  }

  /// @brief Raw const pointer to the storage.
  [[nodiscard]] constexpr auto data() const noexcept -> const value_type* {
    return std::to_address(data_);
  }

  /// @brief Iterator to the first element.
  [[nodiscard]] constexpr auto begin() noexcept -> iterator {
    return iterator{data()};
  }

  /// @brief Const iterator to the first element.
  [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator {
    return const_iterator{data()};
  }

  /// @brief Const iterator to the first element.
  [[nodiscard]] constexpr auto cbegin() const noexcept -> const_iterator {
    return const_iterator{data()};
  }

  /// @brief Iterator past the last element.
  [[nodiscard]] constexpr auto end() noexcept -> iterator {
    return iterator{data() + size_};
  }

  /// @brief Const iterator past the last element.
  [[nodiscard]] constexpr auto end() const noexcept -> const_iterator {
    return const_iterator{data() + size_};
  }

  /// @brief Const iterator past the last element.
  [[nodiscard]] constexpr auto cend() const noexcept -> const_iterator {
    return const_iterator{data() + size_};
  }

  /// @brief Reverse iterator to the last element.
  [[nodiscard]] constexpr auto rbegin() noexcept -> reverse_iterator {
    return reverse_iterator{end()};
  }

  /// @brief Const reverse iterator to the last element.
  [[nodiscard]] constexpr auto rbegin() const noexcept
      -> const_reverse_iterator {
    return const_reverse_iterator{end()};
  }

  /// @brief Const reverse iterator to the last element.
  [[nodiscard]] constexpr auto crbegin() const noexcept
      -> const_reverse_iterator {
    return const_reverse_iterator{cend()};
  }

  /// @brief Reverse iterator before the first element.
  [[nodiscard]] constexpr auto rend() noexcept -> reverse_iterator {
    return reverse_iterator{begin()};
  }

  /// @brief Const reverse iterator before the first element.
  [[nodiscard]] constexpr auto rend() const noexcept -> const_reverse_iterator {
    return const_reverse_iterator{begin()};
  }

  /// @brief Const reverse iterator before the first element.
  [[nodiscard]] constexpr auto crend() const noexcept
      -> const_reverse_iterator {
    return const_reverse_iterator{cbegin()};
  }

  /// @brief Number of elements.
  [[nodiscard]] constexpr auto size() const noexcept -> size_type {
    return size_;
  }

  /// @brief Whether the array holds no elements.
  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return size_ == 0;
  }

  /// @brief Largest representable size.
  [[nodiscard]] constexpr auto max_size() const noexcept -> size_type {
    return MaxSize(alloc_);
  }

  /// @brief View the storage as a mutable span.
  [[nodiscard]] constexpr auto as_span() noexcept -> std::span<T> {
    return std::span<T>{data(), size_};
  }

  /// @brief View the storage as a const span.
  [[nodiscard]] constexpr auto as_span() const noexcept -> std::span<const T> {
    return std::span<const T>{data(), size_};
  }

  /// @brief Swap contents with another array.
  constexpr auto swap(HeapArray& other) noexcept(
      alloc_traits::propagate_on_container_swap::value ||
      alloc_traits::is_always_equal::value) -> void {
    using std::swap;
    swap(data_, other.data_);
    swap(size_, other.size_);
    if constexpr (alloc_traits::propagate_on_container_swap::value) {
      swap(alloc_, other.alloc_);
    }
  }

  /// @brief Equality comparison.
  [[nodiscard]] friend constexpr auto operator==(const HeapArray& a,
                                                 const HeapArray& b) -> bool
    requires std::equality_comparable<T>
  {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }

  /// @brief Lexicographic three-way comparison.
  [[nodiscard]] friend constexpr auto operator<=>(const HeapArray& a,
                                                  const HeapArray& b)
    requires requires(const T& x, const T& y) { detail::SynthThreeWay{}(x, y); }
  {
    return std::lexicographical_compare_three_way(
        a.begin(), a.end(), b.begin(), b.end(), detail::SynthThreeWay{});
  }

 private:
  /// @brief Largest size representable for allocator `a`.
  [[nodiscard]] static constexpr auto MaxSize(const Allocator& a) noexcept
      -> size_type {
    return std::min(
        alloc_traits::max_size(a),
        static_cast<size_type>(std::numeric_limits<difference_type>::max()));
  }

  /// @brief Allocate `n` elements (never `allocate(0)`).
  static constexpr auto AllocateBlock(Allocator& a, size_type n) -> pointer {
    if (n == 0) {
      return pointer{};
    }
    return alloc_traits::allocate(a, n);
  }

  /// @brief Allocate and construct `n` elements.
  template <typename Init>
  static constexpr auto BuildBlock(Allocator& a, size_type n, Init init)
      -> pointer {
    if (n == 0) {
      return pointer{};
    }
    if (n > MaxSize(a)) {
      ThrowOrAbort<std::length_error>("HeapArray: size exceeds max_size()");
    }
    pointer p = AllocateBlock(a, n);
    T* raw = std::to_address(p);
    for (size_type i = 0; i < n; ++i) {
      init(a, raw + i, i);
    }
    return p;
  }

  /// @brief Destroy every element (reverse order) and release the storage.
  constexpr auto DestroyAndDeallocate() noexcept -> void {
    if (size_ == 0) {
      return;
    }
    T* raw = std::to_address(data_);
    for (size_type i = size_; i > 0;) {
      --i;
      alloc_traits::destroy(alloc_, raw + i);
    }
    alloc_traits::deallocate(alloc_, data_, size_);
  }

  pointer data_{nullptr};
  size_type size_{0};
  [[no_unique_address]] Allocator alloc_{};
};

/// @brief Non-member swap for two [`cutils::HeapArray`] objects.
template <typename T, typename A>
constexpr auto swap(HeapArray<T, A>& a,
                    HeapArray<T, A>& b) noexcept(noexcept(a.swap(b))) -> void {
  a.swap(b);
}

/// @brief Deduction guide for the iterator/sentinel constructor.
template <std::forward_iterator It, std::sentinel_for<It> S,
          typename Alloc = std::allocator<std::iter_value_t<It>>>
HeapArray(It, S, Alloc = Alloc()) -> HeapArray<std::iter_value_t<It>, Alloc>;

/// @brief Deduction guide for the `from_range` constructor.
template <std::ranges::input_range R,
          typename Alloc = std::allocator<std::ranges::range_value_t<R>>>
HeapArray(std::from_range_t, R&&, Alloc = Alloc())
    -> HeapArray<std::ranges::range_value_t<R>, Alloc>;

namespace detail {

using HAi = cutils::HeapArray<int>;

static_assert(std::contiguous_iterator<HAi::iterator>);
static_assert(std::contiguous_iterator<HAi::const_iterator>);
static_assert(std::random_access_iterator<HAi::iterator>);
static_assert(std::random_access_iterator<HAi::reverse_iterator>);
static_assert(std::random_access_iterator<HAi::const_reverse_iterator>);
static_assert(std::sized_sentinel_for<HAi::iterator, HAi::iterator>);
static_assert(
    std::equality_comparable_with<HAi::iterator, HAi::const_iterator>);
static_assert(std::totally_ordered_with<HAi::iterator, HAi::const_iterator>);
static_assert(std::sized_sentinel_for<HAi::const_iterator, HAi::iterator>);
static_assert(std::convertible_to<HAi::iterator, HAi::const_iterator>);
static_assert(!std::convertible_to<HAi::const_iterator, HAi::iterator>);
static_assert(std::indirectly_writable<HAi::iterator, int>);
static_assert(std::same_as<std::iter_value_t<HAi::iterator>, int>);
static_assert(std::same_as<
              decltype(std::to_address(std::declval<HAi::iterator>())), int*>);
static_assert(std::ranges::contiguous_range<HAi>);
static_assert(std::ranges::contiguous_range<const HAi>);
static_assert(std::ranges::sized_range<HAi>);
static_assert(std::ranges::common_range<HAi>);
static_assert(std::ranges::random_access_range<HAi>);
static_assert(std::same_as<std::ranges::range_value_t<HAi>, int>);
static_assert(std::ranges::borrowed_range<HAi&>);
static_assert(!std::ranges::borrowed_range<HAi>);
static_assert(std::same_as<decltype(std::declval<HAi&>().data()), int*>);
static_assert(
    std::same_as<decltype(std::declval<const HAi&>().data()), const int*>);
static_assert(std::same_as<HAi::reference, int&>);
static_assert(std::same_as<HAi::const_reference, const int&>);
static_assert(std::same_as<HAi::allocator_type, std::allocator<int>>);
static_assert(std::same_as<HAi::size_type, std::size_t>);
static_assert(std::same_as<HAi::difference_type, std::ptrdiff_t>);
static_assert(
    std::same_as<HAi::reverse_iterator, std::reverse_iterator<HAi::iterator>>);
static_assert(std::copyable<HAi>);
static_assert(std::movable<HAi>);
static_assert(std::is_nothrow_move_constructible_v<HAi>);
static_assert(std::is_nothrow_swappable_v<HAi>);
static_assert(noexcept(std::declval<HAi&>() = std::declval<HAi&&>()));
static_assert(std::three_way_comparable<HAi>);
static_assert(std::equality_comparable<HAi>);
static_assert(noexcept(std::declval<const HAi&>().size()));
static_assert(noexcept(std::declval<const HAi&>().empty()));
static_assert(noexcept(std::declval<HAi&>().begin()));
static_assert(noexcept(std::declval<HAi&>().data()));
static_assert(sizeof(HAi) == sizeof(int*) + sizeof(std::size_t));
using MO = cutils::HeapArray<std::unique_ptr<int>>;
static_assert(std::movable<MO>);
static_assert(!std::copyable<MO>);
static_assert(!std::copy_constructible<MO>);
struct NoDefault {
  NoDefault() = delete;
  explicit NoDefault(int) {}
};
static_assert(
    !std::constructible_from<cutils::HeapArray<NoDefault>, std::size_t>);

static_assert(
    std::same_as<decltype(cutils::HeapArray{
                     std::from_range, std::declval<std::vector<double>&>()}),
                 cutils::HeapArray<double>>);

}  // namespace detail

}  // namespace cutils

#endif  // CUTILS_COLLECTIONS_HEAP_ARRAY_HPP
