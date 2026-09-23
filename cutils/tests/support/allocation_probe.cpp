// See LICENSE in the repository root.

#include <atomic>
#include <cassert>
#include <cstddef>

extern "C" auto __sanitizer_install_malloc_and_free_hooks(
    void (*allocate)(const volatile void*, std::size_t),
    void (*release)(const volatile void*)) -> int;

namespace {
constinit std::atomic<bool> tracking{false};
constinit std::atomic<std::size_t> allocations{0};

void RecordAllocation(const volatile void*, std::size_t) noexcept {
  if (tracking.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordFree(const volatile void*) noexcept {}
}

extern "C" void cpplap_begin_allocation_check() {
  static const int installed =
      __sanitizer_install_malloc_and_free_hooks(RecordAllocation, RecordFree);
  assert(installed != 0);
  allocations.store(0);
  tracking.store(true);
}
extern "C" auto cpplap_end_allocation_check() -> std::size_t {
  tracking.store(false);
  return allocations.load();
}
