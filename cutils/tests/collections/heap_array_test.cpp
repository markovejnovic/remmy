// See LICENSE in the repository root.

#include <algorithm>
#include <array>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cutils/collections/heap_array.hpp>
#include <forward_list>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using cutils::HeapArray;

template <typename T>
[[nodiscard]] auto AsRef(T& x) noexcept -> T& {
  return x;
}

struct AllocStats {
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
  std::size_t outstanding = 0;
  struct Block {
    std::size_t count;
    int owner;
  };
  std::unordered_map<void*, Block> blocks;
  bool incompatible_owner = false;
};

template <typename T, bool Pocca, bool Pocma, bool Pocs, bool AlwaysEq>
class CAlloc {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::bool_constant<Pocca>;
  using propagate_on_container_move_assignment = std::bool_constant<Pocma>;
  using propagate_on_container_swap = std::bool_constant<Pocs>;
  using is_always_equal = std::bool_constant<AlwaysEq>;

  static constexpr int kSocccId = 77;

  template <typename U>
  struct rebind {
    using other = CAlloc<U, Pocca, Pocma, Pocs, AlwaysEq>;
  };

  CAlloc(AllocStats& stats, int id) noexcept : stats_(&stats), id_(id) {}

  template <typename U>
  explicit CAlloc(const CAlloc<U, Pocca, Pocma, Pocs, AlwaysEq>& other) noexcept
      : stats_(other.stats_), id_(other.id_) {}

  [[nodiscard]] auto allocate(std::size_t n) -> T* {
    ++stats_->allocations;
    stats_->outstanding += n;
    auto* pointer = std::allocator<T>{}.allocate(n);
    stats_->blocks.emplace(pointer, AllocStats::Block{n, AlwaysEq ? 0 : id_});
    return pointer;
  }

  auto deallocate(T* p, std::size_t n) noexcept -> void {
    const auto block = stats_->blocks.find(p);
    if (block == stats_->blocks.end() || block->second.count != n ||
        block->second.owner != (AlwaysEq ? 0 : id_)) {
      stats_->incompatible_owner = true;
    }
    if (block != stats_->blocks.end()) {
      stats_->blocks.erase(block);
    }
    ++stats_->deallocations;
    stats_->outstanding -= n;
    std::allocator<T>{}.deallocate(p, n);
  }

  [[nodiscard]] auto select_on_container_copy_construction() const -> CAlloc {
    return CAlloc(*stats_, kSocccId);
  }

  [[nodiscard]] auto id() const noexcept -> int { return id_; }
  [[nodiscard]] auto stats() const noexcept -> AllocStats* { return stats_; }

  template <typename U>
  [[nodiscard]] auto operator==(
      const CAlloc<U, Pocca, Pocma, Pocs, AlwaysEq>& other) const noexcept
      -> bool {
    if constexpr (AlwaysEq) {
      return true;
    } else {
      return stats_ == other.stats_ && id_ == other.id_;
    }
  }

 private:
  template <typename, bool, bool, bool, bool>
  friend class CAlloc;

  AllocStats* stats_;
  int id_;
};

template <typename T>
using PropagateAlloc = CAlloc<T, true, true, true, false>;
template <typename T>
using StickyAlloc = CAlloc<T, false, false, false, false>;
template <typename T>
using AlwaysEqAlloc = CAlloc<T, false, false, false, true>;

template <typename T>
class SmallMaxAlloc {
 public:
  using value_type = T;
  static constexpr std::size_t kMax = 8;

  template <typename U>
  struct rebind {
    using other = SmallMaxAlloc<U>;
  };

  SmallMaxAlloc() noexcept = default;
  template <typename U>
  explicit SmallMaxAlloc(const SmallMaxAlloc<U>&) noexcept {}

  [[nodiscard]] auto allocate(std::size_t n) -> T* {
    return std::allocator<T>{}.allocate(n);
  }
  auto deallocate(T* p, std::size_t n) noexcept -> void {
    std::allocator<T>{}.deallocate(p, n);
  }
  [[nodiscard]] auto max_size() const noexcept -> std::size_t { return kMax; }

  template <typename U>
  [[nodiscard]] auto operator==(const SmallMaxAlloc<U>&) const noexcept
      -> bool {
    return true;
  }
};

struct Tracked {
  inline static std::int64_t ctor = 0;
  inline static std::int64_t dtor = 0;
  inline static std::int64_t alive = 0;

  int value{0};

  static auto Reset() -> void {
    ctor = 0;
    dtor = 0;
    alive = 0;
  }

  Tracked() noexcept {
    ++ctor;
    ++alive;
  }
  explicit Tracked(int v) noexcept : value(v) {
    ++ctor;
    ++alive;
  }
  Tracked(const Tracked& o) noexcept : value(o.value) {
    ++ctor;
    ++alive;
  }
  Tracked(Tracked&& o) noexcept : value(o.value) {
    o.value = -1;
    ++ctor;
    ++alive;
  }
  auto operator=(const Tracked&) noexcept -> Tracked& = default;
  auto operator=(Tracked&&) noexcept -> Tracked& = default;
  ~Tracked() {
    ++dtor;
    --alive;
  }

  [[nodiscard, maybe_unused]] friend auto operator==(const Tracked&,
                                                     const Tracked&) noexcept
      -> bool = default;
};

struct MoveOnly {
  int value{0};
  MoveOnly() = default;
  explicit MoveOnly(int v) : value(v) {}
  MoveOnly(const MoveOnly&) = delete;
  auto operator=(const MoveOnly&) -> MoveOnly& = delete;
  MoveOnly(MoveOnly&& o) noexcept : value(std::exchange(o.value, 0)) {}
  auto operator=(MoveOnly&& o) noexcept -> MoveOnly& {
    value = std::exchange(o.value, 0);
    return *this;
  }
  ~MoveOnly() = default;
};

struct NoDefault {
  int value;
  NoDefault() = delete;
  explicit NoDefault(int v) : value(v) {}
};

struct alignas(64) Over {
  int value{0};
  Over() = default;
  explicit Over(int v) : value(v) {}
};

struct OnlyLess {
  int value{0};
  explicit OnlyLess(int v) : value(v) {}
  [[nodiscard]] friend auto operator<(const OnlyLess& a,
                                      const OnlyLess& b) noexcept -> bool {
    return a.value < b.value;
  }
};

struct OrderRec {
  inline static std::vector<int> order;
  int value{0};
  OrderRec() = default;
  explicit OrderRec(int v) : value(v) {}
  OrderRec(const OrderRec&) = default;
  OrderRec(OrderRec&&) = default;
  auto operator=(const OrderRec&) -> OrderRec& = default;
  auto operator=(OrderRec&&) -> OrderRec& = default;
  ~OrderRec() { order.push_back(value); }
};

using HAi = HeapArray<int>;

static_assert(std::constructible_from<HeapArray<NoDefault>, const NoDefault*,
                                      const NoDefault*>);

template <typename R>
concept FromRangeCtor =
    requires(R r) { HeapArray<int>(std::from_range, std::move(r)); };
using IStreamView =
    decltype(std::views::istream<int>(std::declval<std::istringstream&>()));
static_assert(!FromRangeCtor<IStreamView>);
static_assert(FromRangeCtor<std::vector<int>>);

using HAol = HeapArray<OnlyLess>;
static_assert(std::same_as<decltype(std::declval<const HAol&>() <=>
                                    std::declval<const HAol&>()),
                           std::weak_ordering>);
static_assert(!std::equality_comparable<HAol>);

static_assert(std::same_as<decltype(std::declval<const HAi&>() <=>
                                    std::declval<const HAi&>()),
                           std::strong_ordering>);

using StickyArr = HeapArray<int, StickyAlloc<int>>;
static_assert(
    !noexcept(std::declval<StickyArr&>() = std::declval<StickyArr&&>()));
static_assert(!std::is_nothrow_swappable_v<StickyArr>);

using PropArr = HeapArray<int, PropagateAlloc<int>>;
static_assert(std::is_nothrow_swappable_v<PropArr>);

template <typename S, typename Arg>
concept SpanBindable = requires(Arg&& a) { S(std::forward<Arg>(a)); };
static_assert(SpanBindable<std::span<int>, HeapArray<int>&>);
static_assert(SpanBindable<std::span<const int>, const HeapArray<int>&>);
static_assert(!SpanBindable<std::span<int>, HeapArray<int>>);

// Every operation stays usable in constant evaluation.
static_assert([] consteval -> bool {
  HeapArray<int> a{5, 3, 1, 4, 2};
  std::ranges::sort(a);
  bool ok = std::ranges::equal(a, std::array{1, 2, 3, 4, 5});
  HeapArray<int> b{9, 9};
  b = a;
  ok = ok && (b == a);
  HeapArray<int> c{0};
  c = std::move(b);
  ok = ok && b.empty() && (c.size() == 5) && (c.at(0) == 1) && (c[2] == 3);
  std::ranges::reverse(c);
  ok = ok && (c.front() == 5) && (c.back() == 1);
  c = {4, 5};
  const HeapArray<int> d{4, 5};
  ok = ok && ((c <=> d) == std::strong_ordering::equal);
  ok = ok && ((c <=> a) == std::strong_ordering::greater);
  return ok;
}());

using CopyAlloc = CAlloc<Tracked, true, false, false, false>;
using MoveAlloc = CAlloc<Tracked, false, true, false, false>;
using SwapAlloc = CAlloc<Tracked, false, false, true, false>;

void CheckBalanced(const AllocStats& stats) {
  CHECK_FALSE(stats.incompatible_owner);
  CHECK(stats.blocks.empty());
  CHECK(stats.allocations == stats.deallocations);
  CHECK(stats.outstanding == 0);
  CHECK(Tracked::alive == 0);
  CHECK(Tracked::ctor == Tracked::dtor);
}

auto IsAligned(const void* pointer) -> bool {
  return reinterpret_cast<std::uintptr_t>(pointer) % alignof(Over) == 0;
}

SCENARIO("An empty HeapArray owns no storage", "[heap_array]") {
  GIVEN("a default-constructed array") {
    const HeapArray<int> a;
    THEN("it is empty with no storage") {
      REQUIRE(a.empty());
      REQUIRE(a.data() == nullptr);
      REQUIRE(a.begin() == a.end());
    }
  }
  GIVEN("an array constructed from only an allocator") {
    const HeapArray<int> a{std::allocator<int>{}};
    THEN("it is empty with no storage") {
      REQUIRE(a.empty());
      REQUIRE(a.data() == nullptr);
    }
  }
}

SCENARIO("HeapArray constructors fill the array from their arguments",
         "[heap_array]") {
  GIVEN("an element count") {
    WHEN("an array of that many elements is constructed") {
      const HeapArray<int> a(4);
      THEN("every element is value-initialised") {
        REQUIRE(a.size() == 4);
        REQUIRE(std::ranges::all_of(a, [](int x) { return x == 0; }));
      }
    }
  }
  GIVEN("an element count and a value") {
    WHEN("an array is constructed from them") {
      const HeapArray<int> a(3, 9);
      THEN("it holds that many copies of the value") {
        REQUIRE(std::ranges::equal(a, std::array{9, 9, 9}));
      }
    }
  }
  GIVEN("an iterator pair") {
    const std::array src{1, 2, 3, 4};
    WHEN("an array is constructed from it") {
      const HeapArray a(src.begin(), src.end());
      static_assert(std::same_as<decltype(a), const HeapArray<int>>);
      THEN("the element type is deduced and the elements are copied") {
        REQUIRE(std::ranges::equal(a, src));
      }
    }
  }
  GIVEN("a sized range") {
    const std::vector<int> src{7, 8, 9};
    WHEN("an array is constructed from it with from_range") {
      const HeapArray a(std::from_range, src);
      THEN("the elements are copied") { REQUIRE(std::ranges::equal(a, src)); }
    }
  }
  GIVEN("an unsized forward range") {
    const std::forward_list<int> src{2, 4, 6, 8};
    WHEN("an array is constructed from it with from_range") {
      const HeapArray<int> a(std::from_range, src);
      THEN("its length is measured and the elements are copied") {
        REQUIRE(a.size() == 4);
        REQUIRE(std::ranges::equal(a, src));
      }
    }
  }
  GIVEN("an initialiser list") {
    WHEN("an array is constructed from it") {
      const HeapArray<int> a{10, 20, 30};
      THEN("the elements are copied") {
        REQUIRE(std::ranges::equal(a, std::array{10, 20, 30}));
      }
    }
  }
  GIVEN("move iterators over move-only elements") {
    std::array<MoveOnly, 3> src{MoveOnly{1}, MoveOnly{2}, MoveOnly{3}};
    WHEN("an array is constructed from them") {
      const HeapArray<MoveOnly> a(std::make_move_iterator(src.begin()),
                                  std::make_move_iterator(src.end()));
      THEN("the elements are moved in") {
        REQUIRE(a.size() == 3);
        REQUIRE((a[0].value == 1 && a[2].value == 3));
        REQUIRE(src[0].value == 0);
      }
    }
  }
  GIVEN("elements that are not default-constructible") {
    const std::array src{NoDefault{7}, NoDefault{8}, NoDefault{9}};
    WHEN("an array is constructed from an iterator pair over them") {
      const HeapArray<NoDefault> a(src.begin(), src.end());
      THEN("the elements are copied") {
        REQUIRE(a.size() == 3);
        REQUIRE(a[1].value == 8);
      }
    }
  }
}

SCENARIO("Empty HeapArrays never touch the allocator", "[heap_array]") {
  GIVEN("a counting allocator") {
    AllocStats stats;
    using A = StickyAlloc<int>;
    WHEN("arrays are built, copied and moved from every kind of empty source") {
      {
        HeapArray<int, A> a(std::size_t{0}, A(stats, 1));
        const HeapArray<int, A> b(std::size_t{0}, 7, A(stats, 1));
        const std::array<int, 0> empty_src{};
        const HeapArray<int, A> c(empty_src.begin(), empty_src.end(),
                                  A(stats, 1));
        const std::vector<int> empty_vec;
        const HeapArray<int, A> d(std::from_range, empty_vec, A(stats, 1));
        const HeapArray<int, A> e(std::initializer_list<int>{}, A(stats, 1));
        const HeapArray<int, A> f = a;
        const HeapArray<int, A> g = std::move(a);
        REQUIRE((a.empty() && b.empty() && c.empty() && d.empty()));
        REQUIRE((e.empty() && f.empty() && g.empty()));
        REQUIRE((a.data() == nullptr && f.data() == nullptr &&
                 g.data() == nullptr));
      }
      THEN("nothing was allocated or deallocated") {
        REQUIRE(stats.allocations == 0);
        REQUIRE(stats.deallocations == 0);
        REQUIRE(stats.blocks.empty());
        REQUIRE_FALSE(stats.incompatible_owner);
      }
    }
  }
}

SCENARIO("Copying a HeapArray makes an independent deep copy", "[heap_array]") {
  GIVEN("an array with elements") {
    HeapArray<int> a{1, 2, 3};

    WHEN("it is copy-constructed") {
      HeapArray<int> b = a;
      THEN("the copy has equal elements in separate storage") {
        REQUIRE(b == a);
        REQUIRE(b.data() != a.data());
      }
      AND_WHEN("the copy is modified") {
        b[0] = 99;
        THEN("the original is unchanged") { REQUIRE(a[0] == 1); }
      }
    }
    WHEN("it is copy-assigned to an array of a different size") {
      HeapArray<int> b{9, 9, 9, 9, 9};
      b = a;
      THEN("the destination takes its size and elements in separate storage") {
        REQUIRE(b == a);
        REQUIRE(b.data() != a.data());
      }
    }
    WHEN("it is copy-assigned to itself") {
      a = AsRef(a);
      THEN("it is unchanged") {
        REQUIRE(std::ranges::equal(a, std::array{1, 2, 3}));
      }
    }
  }
}

SCENARIO("Copy construction selects the allocator's copy-construction choice",
         "[heap_array]") {
  GIVEN("an array using a sticky allocator") {
    using A = StickyAlloc<int>;
    AllocStats stats;
    {
      const HeapArray<int, A> src(3, 4, A(stats, 1));
      WHEN("it is copy-constructed") {
        const HeapArray<int, A> cpy = src;
        THEN("the copy uses select_on_container_copy_construction") {
          REQUIRE(cpy.get_allocator().id() == A::kSocccId);
          REQUIRE(std::ranges::equal(cpy, src));
        }
      }
    }
    CheckBalanced(stats);
  }
}

SCENARIO("Moving a HeapArray transfers its storage", "[heap_array]") {
  GIVEN("an array with elements") {
    HeapArray<int> a{1, 2, 3};
    const int* storage = a.data();

    WHEN("it is move-constructed") {
      const HeapArray<int> b = std::move(a);
      THEN("the new array owns the storage and the source is empty") {
        REQUIRE(b.data() == storage);
        REQUIRE(b.size() == 3);
        REQUIRE(a.empty());
        REQUIRE(a.data() == nullptr);
      }
    }
    WHEN("it is move-assigned to a non-empty array") {
      HeapArray<int> b{9, 9};
      b = std::move(a);
      THEN("the destination owns the storage and the source is empty") {
        REQUIRE(b.data() == storage);
        REQUIRE(b.size() == 3);
        REQUIRE(a.empty());
        REQUIRE(a.data() == nullptr);
      }
    }
    WHEN("it is move-assigned to itself") {
      a = std::move(AsRef(a));
      THEN("it keeps its storage and elements") {
        REQUIRE(a.data() == storage);
        REQUIRE(std::ranges::equal(a, std::array{1, 2, 3}));
      }
    }
  }
}

SCENARIO("Initialiser-list assignment replaces the contents", "[heap_array]") {
  GIVEN("a one-element array") {
    HeapArray<int> a{0};
    WHEN("a longer list is assigned") {
      a = {5, 6, 7, 8, 9};
      THEN("it holds exactly the list") {
        REQUIRE(std::ranges::equal(a, std::array{5, 6, 7, 8, 9}));
      }
    }
  }
}

SCENARIO("Element accessors address the underlying storage", "[heap_array]") {
  GIVEN("an array and a const view of it") {
    HeapArray<int> a{0, 0, 0, 0};
    const HeapArray<int>& ca = a;
    WHEN("elements are written through front, operator[], at and back") {
      a.front() = 1;
      a[1] = 2;
      a.at(2) = 3;
      a.back() = 4;
      THEN("every const accessor reads the written values") {
        REQUIRE(ca.front() == 1);
        REQUIRE(ca[1] == 2);
        REQUIRE(ca.at(2) == 3);
        REQUIRE(ca.back() == 4);
        REQUIRE(ca.data()[3] == 4);
      }
    }
  }
}

SCENARIO("HeapArray iterators traverse the storage", "[heap_array]") {
  GIVEN("an array of distinct elements") {
    HeapArray<int> a{4, 1, 5, 2, 3};
    const HeapArray<int>& ca = a;

    THEN("iterators address the storage and mix with const iterators") {
      REQUIRE(std::to_address(a.begin() + 2) == a.data() + 2);
      REQUIRE(a.begin() == a.cbegin());
      REQUIRE(a.begin() < a.cend());
      REQUIRE((a.cend() - a.begin()) == 5);
    }
    THEN("reverse iterators walk the elements backwards") {
      REQUIRE(std::ranges::equal(std::ranges::subrange(a.rbegin(), a.rend()),
                                 std::array{3, 2, 5, 1, 4}));
      REQUIRE(
          std::ranges::equal(std::ranges::subrange(ca.crbegin(), ca.crend()),
                             std::array{3, 2, 5, 1, 4}));
    }
    WHEN("it is sorted through its iterators") {
      std::sort(a.begin(), a.end());
      THEN("the elements are in order") {
        REQUIRE(std::ranges::equal(a, std::array{1, 2, 3, 4, 5}));
      }
    }
  }
}

SCENARIO("HeapArray iterators support random-access arithmetic",
         "[heap_array]") {
  GIVEN("an array whose elements equal their indices") {
    HeapArray<int> a{0, 1, 2, 3, 4};
    const auto begin = a.begin();

    THEN("subscripting and offsetting reach the expected elements") {
      REQUIRE(begin[2] == 2);
      REQUIRE((2 + begin) == begin + 2);
      REQUIRE(((begin + 3) - 3) == begin);
      REQUIRE(*(a.end() - 2) == 3);
    }
    WHEN("an iterator is post-incremented") {
      auto it = begin;
      const auto previous = it++;
      THEN("it advances and returns its old position") {
        REQUIRE(previous == begin);
        REQUIRE(it == begin + 1);
      }
      AND_WHEN("it is post-decremented") {
        const auto advanced = it--;
        THEN("it retreats and returns its old position") {
          REQUIRE(advanced == begin + 1);
          REQUIRE(it == begin);
        }
      }
    }
    WHEN("an iterator is compound-advanced and retreated") {
      auto it = begin;
      it += 3;
      REQUIRE(it == begin + 3);
      it -= 2;
      THEN("it lands on the net offset") { REQUIRE(*it == 1); }
    }
  }
}

SCENARIO("Spans alias a HeapArray's storage", "[heap_array]") {
  GIVEN("an array") {
    HeapArray<int> a{1, 2, 3, 4};
    WHEN("it is viewed through as_span and implicit span conversion") {
      const std::span<int> s = a.as_span();
      const std::span<int> bound = a;
      const std::span<const int> cs = std::as_const(a).as_span();
      THEN("every view covers the same storage") {
        REQUIRE((s.data() == a.data() && s.size() == 4));
        REQUIRE((bound.data() == a.data() && bound.size() == 4));
        REQUIRE((cs.data() == a.data() && cs.size() == 4));
      }
      AND_WHEN("an element is written through the span") {
        s[0] = 100;
        THEN("the array sees the write") { REQUIRE(a[0] == 100); }
      }
    }
  }
}

SCENARIO("HeapArrays compare lexicographically", "[heap_array]") {
  GIVEN("arrays of strongly ordered elements") {
    const HeapArray<int> a{1, 2, 3};
    const HeapArray<int> same{1, 2, 3};
    const HeapArray<int> larger{1, 2, 4};
    const HeapArray<int> prefix{1, 2};
    const HeapArray<int> empty1;
    const HeapArray<int> empty2;
    THEN("equality and ordering follow the elements, then the length") {
      REQUIRE(a == same);
      REQUIRE(a != larger);
      REQUIRE((a <=> same) == std::strong_ordering::equal);
      REQUIRE((a <=> larger) == std::strong_ordering::less);
      REQUIRE((larger <=> a) == std::strong_ordering::greater);
      REQUIRE((prefix <=> a) == std::strong_ordering::less);
      REQUIRE((empty1 <=> a) == std::strong_ordering::less);
      REQUIRE(empty1 == empty2);
    }
  }
  GIVEN("arrays of elements that only define operator<") {
    const HeapArray<OnlyLess> x{OnlyLess{1}, OnlyLess{2}};
    const HeapArray<OnlyLess> same{OnlyLess{1}, OnlyLess{2}};
    const HeapArray<OnlyLess> larger{OnlyLess{1}, OnlyLess{3}};
    const HeapArray<OnlyLess> empty1;
    const HeapArray<OnlyLess> empty2;
    THEN("ordering is synthesised as a weak ordering") {
      REQUIRE((x <=> larger) == std::weak_ordering::less);
      REQUIRE((larger <=> x) == std::weak_ordering::greater);
      REQUIRE((x <=> same) == std::weak_ordering::equivalent);
      REQUIRE((empty1 <=> empty2) == std::weak_ordering::equivalent);
    }
  }
}

SCENARIO("Destroying a HeapArray destroys every element in reverse order",
         "[heap_array]") {
  GIVEN("an array of lifetime-tracked elements") {
    Tracked::Reset();
    WHEN("it leaves scope") {
      {
        const HeapArray<Tracked> a(4);
        REQUIRE(Tracked::alive == 4);
      }
      THEN("every constructed element was destroyed") {
        REQUIRE(Tracked::alive == 0);
        REQUIRE(Tracked::ctor == Tracked::dtor);
      }
    }
  }
  GIVEN("an array of elements that record their destruction") {
    WHEN("it leaves scope") {
      {
        const HeapArray<OrderRec> a{OrderRec{0}, OrderRec{1}, OrderRec{2},
                                    OrderRec{3}};
        OrderRec::order.clear();
      }
      THEN("the elements were destroyed last to first") {
        REQUIRE(OrderRec::order == std::vector<int>{3, 2, 1, 0});
      }
    }
  }
}

SCENARIO("Over-aligned elements are stored at their alignment",
         "[heap_array]") {
  GIVEN("an array of 64-byte-aligned elements") {
    const HeapArray<Over> a(5, Over{3});
    THEN("every element is aligned and initialised") {
      REQUIRE(IsAligned(a.data()));
      for (const auto& e : a) {
        REQUIRE(IsAligned(&e));
        REQUIRE(e.value == 3);
      }
    }
  }
}

SCENARIO("max_size is bounded by the allocator and difference_type",
         "[heap_array]") {
  GIVEN("an array of bytes using the standard allocator") {
    const HeapArray<char> a;
    THEN("max_size is clamped to the largest difference_type") {
      REQUIRE(a.max_size() == static_cast<std::size_t>(
                                  std::numeric_limits<std::ptrdiff_t>::max()));
    }
  }
  GIVEN("an allocator with a small max_size") {
    const HeapArray<int, SmallMaxAlloc<int>> a;
    THEN("max_size is the allocator's") {
      REQUIRE(a.max_size() == SmallMaxAlloc<int>::kMax);
    }
  }
}

// Catch2 has no templated SCENARIO; "Scenario: " matches what SCENARIO emits.
// Each pass through a scenario destroys its arrays before CheckBalanced runs.

TEMPLATE_TEST_CASE(
    "Scenario: Copy assignment follows the allocator's propagation trait",
    "[heap_array]", CopyAlloc, StickyAlloc<Tracked>) {
  using A = TestType;
  const int source_id = GENERATE(1, 2);
  CAPTURE(source_id);
  AllocStats stats;
  Tracked::Reset();
  GIVEN("a destination and a source with elements") {
    {
      HeapArray<Tracked, A> destination(2, Tracked{1}, A(stats, 1));
      const HeapArray<Tracked, A> source(3, Tracked{7}, A(stats, source_id));
      const auto allocations = stats.allocations;
      WHEN("the source is copy-assigned to the destination") {
        destination = source;
        THEN("the destination holds a copy in one new block") {
          REQUIRE(destination == source);
          REQUIRE(destination.data() != source.data());
          REQUIRE(stats.allocations == allocations + 1);
          REQUIRE(Tracked::alive == 6);
        }
        THEN("the allocator propagates only when the trait says so") {
          REQUIRE(destination.get_allocator().id() ==
                  (A::propagate_on_container_copy_assignment::value ? source_id
                                                                    : 1));
        }
        AND_WHEN("the copy is modified") {
          destination[0].value = 9;
          THEN("the source is unchanged") { REQUIRE(source[0].value == 7); }
        }
      }
    }
    CheckBalanced(stats);
  }
}

TEMPLATE_TEST_CASE(
    "Scenario: Move assignment steals storage only when allocators allow",
    "[heap_array]", MoveAlloc, StickyAlloc<Tracked>, AlwaysEqAlloc<Tracked>) {
  using A = TestType;
  int source_id = 2;
  if constexpr (std::same_as<A, StickyAlloc<Tracked>>) {
    source_id = GENERATE(1, 2);
  }
  CAPTURE(source_id);
  const bool steals = A::propagate_on_container_move_assignment::value ||
                      A::is_always_equal::value || source_id == 1;
  AllocStats stats;
  Tracked::Reset();
  GIVEN("a destination and a source with elements") {
    {
      HeapArray<Tracked, A> destination(2, Tracked{1}, A(stats, 1));
      HeapArray<Tracked, A> source(3, Tracked{7}, A(stats, source_id));
      const auto* storage = source.data();
      const auto allocations = stats.allocations;
      const auto constructions = Tracked::ctor;
      WHEN("the source is move-assigned to the destination") {
        destination = std::move(source);
        THEN("the destination holds the elements and the source is empty") {
          REQUIRE(destination.size() == 3);
          REQUIRE(destination[0].value == 7);
          REQUIRE(destination[2].value == 7);
          REQUIRE(source.empty());
          REQUIRE(source.data() == nullptr);
          REQUIRE(Tracked::alive == 3);
        }
        THEN("the allocator propagates only when the trait says so") {
          REQUIRE(destination.get_allocator().id() ==
                  (A::propagate_on_container_move_assignment::value ? source_id
                                                                    : 1));
        }
        THEN("storage is stolen, or elements are moved into a new block") {
          REQUIRE(stats.allocations == allocations + (steals ? 0UZ : 1UZ));
          REQUIRE(Tracked::ctor == constructions + (steals ? 0 : 3));
          REQUIRE((destination.data() == storage) == steals);
        }
      }
    }
    CheckBalanced(stats);
  }
}

TEMPLATE_TEST_CASE("Scenario: Move construction honours the given allocator",
                   "[heap_array]", StickyAlloc<Tracked>,
                   AlwaysEqAlloc<Tracked>) {
  using A = TestType;
  static_assert(std::is_nothrow_constructible_v<HeapArray<Tracked, A>,
                                                HeapArray<Tracked, A>&&, A> ==
                A::is_always_equal::value);
  AllocStats stats;
  Tracked::Reset();
  GIVEN("a source array with elements") {
    {
      HeapArray<Tracked, A> source(3, Tracked{7}, A(stats, 1));
      const auto* storage = source.data();
      const auto allocations = stats.allocations;
      const auto constructions = Tracked::ctor;

      WHEN("it is move-constructed") {
        const HeapArray<Tracked, A> destination(std::move(source));
        THEN("the storage and allocator move without touching elements") {
          REQUIRE(destination.data() == storage);
          REQUIRE(destination.get_allocator().id() == 1);
          REQUIRE(source.empty());
          REQUIRE(source.data() == nullptr);
          REQUIRE(stats.allocations == allocations);
          REQUIRE(Tracked::ctor == constructions);
        }
      }
      WHEN("it is move-constructed with an explicit allocator") {
        int destination_id = 2;
        if constexpr (std::same_as<A, StickyAlloc<Tracked>>) {
          destination_id = GENERATE(1, 2);
        }
        CAPTURE(destination_id);
        const bool steals = A::is_always_equal::value || destination_id == 1;
        const HeapArray<Tracked, A> destination(std::move(source),
                                                A(stats, destination_id));
        THEN("the destination uses that allocator and holds the elements") {
          REQUIRE(destination.get_allocator().id() == destination_id);
          REQUIRE(destination.size() == 3);
          REQUIRE(destination[0].value == 7);
          REQUIRE(destination[2].value == 7);
        }
        if (steals) {
          THEN("equal allocators let it steal the storage") {
            REQUIRE(destination.data() == storage);
            REQUIRE(source.empty());
            REQUIRE(stats.allocations == allocations);
            REQUIRE(Tracked::ctor == constructions);
          }
        } else {
          THEN("unequal allocators move each element into a new block") {
            REQUIRE(destination.data() != storage);
            REQUIRE(stats.allocations == allocations + 1);
            REQUIRE(Tracked::ctor == constructions + 3);
            REQUIRE(source.size() == 3);
            REQUIRE(source[0].value == -1);
            REQUIRE(source[2].value == -1);
          }
        }
      }
    }
    CheckBalanced(stats);
  }
}

TEMPLATE_TEST_CASE("Scenario: Swapping exchanges storage without copying",
                   "[heap_array]", SwapAlloc, StickyAlloc<Tracked>) {
  using A = TestType;
  // Swapping without propagation requires equal allocators.
  constexpr int kOtherId = A::propagate_on_container_swap::value ? 2 : 1;
  AllocStats stats;
  Tracked::Reset();
  GIVEN("two arrays of different sizes") {
    {
      HeapArray<Tracked, A> first(2, Tracked{1}, A(stats, 1));
      HeapArray<Tracked, A> second(3, Tracked{7}, A(stats, kOtherId));
      const auto* first_storage = first.data();
      const auto* second_storage = second.data();
      const auto allocations = stats.allocations;
      const auto constructions = Tracked::ctor;
      WHEN("they are swapped") {
        swap(first, second);
        THEN("storage, sizes and allocators are exchanged") {
          REQUIRE(first.data() == second_storage);
          REQUIRE(second.data() == first_storage);
          REQUIRE(first.size() == 3);
          REQUIRE(second.size() == 2);
          REQUIRE(first.get_allocator().id() == kOtherId);
          REQUIRE(second.get_allocator().id() == 1);
        }
        THEN("nothing was allocated or constructed") {
          REQUIRE(stats.allocations == allocations);
          REQUIRE(Tracked::ctor == constructions);
        }
      }
    }
    CheckBalanced(stats);
  }
}
}
