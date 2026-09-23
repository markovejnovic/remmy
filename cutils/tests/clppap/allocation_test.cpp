// See LICENSE in the repository root.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cutils/clppap/clppap.hpp>
#include <new>

extern "C" void cpplap_begin_allocation_check();
extern "C" auto cpplap_end_allocation_check() -> std::size_t;

namespace {
struct[[= cpplap::Help("allocation check")]] Cli {
  [[ = cpplap::Short("-r"), = cpplap::Long("--recursive") ]] bool recursive =
      false;
  [[= cpplap::Positional]] cpplap::PositionalIterator paths;
};
struct Flags {
  [[= cpplap::Short("-r")]] bool recursive = false;
};

/// Proves the probe sees every allocation entry point, so a zero count from it
/// cannot pass vacuously.
void RequireProbeCountsAllocations() {
  void* (*volatile allocate)(std::size_t) = std::malloc;
  void* (*volatile zero_allocate)(std::size_t, std::size_t) = std::calloc;
  void* (*volatile resize)(void*, std::size_t) = std::realloc;
  void* (*volatile aligned)(std::size_t, std::size_t) = std::aligned_alloc;
  int (*volatile posix)(void**, std::size_t, std::size_t) = posix_memalign;
  void* (*volatile cpp_allocate)(std::size_t) = ::operator new;
  void* (*volatile cpp_array_allocate)(std::size_t) = ::operator new[];
  void* (*volatile cpp_aligned)(std::size_t, std::align_val_t) = ::operator new;

  const auto check = [](const char* id, auto alloc, auto release) {
    CAPTURE(id);
    cpplap_begin_allocation_check();
    void* pointer = alloc();
    const auto count = cpplap_end_allocation_check();
    const bool allocated = pointer != nullptr;
    release(pointer);
    REQUIRE(allocated);
    REQUIRE(count > 0);
  };
  check("malloc", [&] { return allocate(16); }, std::free);
  check("calloc", [&] { return zero_allocate(2, 16); }, std::free);
  void* pointer = std::malloc(16);
  REQUIRE(pointer != nullptr);
  check("realloc", [&] { return resize(pointer, 32); }, std::free);
  check("aligned_alloc", [&] { return aligned(64, 64); }, std::free);
  int status = -1;
  check(
      "posix_memalign",
      [&] {
        void* result = nullptr;
        status = posix(&result, 64, 64);
        return result;
      },
      std::free);
  REQUIRE(status == 0);
  check(
      "operator new", [&] { return cpp_allocate(16); },
      [](void* p) { ::operator delete(p); });
  check(
      "operator new[]", [&] { return cpp_array_allocate(16); },
      [](void* p) { ::operator delete[](p); });
  check(
      "aligned operator new",
      [&] { return cpp_aligned(64, std::align_val_t{64}); },
      [](void* p) { ::operator delete(p, std::align_val_t{64}); });
}
}

SCENARIO("Parsing never allocates", "[clppap][allocation]") {
  GIVEN("an allocation probe that observes every allocator") {
    RequireProbeCountsAllocations();

    AND_GIVEN("a huge argv, an oversized unknown option and misuse") {
      std::array<const char*, 4097> args{};
      args.fill("file");
      args[0] = "remmy";
      args[200] = "--recursive";
      std::array<char, 16384> long_option{};
      long_option.fill('x');
      long_option[0] = '-';
      long_option.back() = '\0';
      const char* invalid[]{"remmy", long_option.data()};
      const char* operand[]{"remmy", "file"};

      WHEN("every parse path runs, including help and error teardown") {
        cpplap_begin_allocation_check();
        auto parsed =
            cpplap::Parse<Cli>(static_cast<int>(args.size()), args.data());
        const bool recursive = parsed && parsed->recursive;
        std::size_t paths = 0;
        if (parsed) {
          for ([[maybe_unused]] const char* path : parsed->paths) {
            ++paths;
          }
        }
        const auto error = cpplap::Parse<Cli>(2, invalid);
        const auto rejected = cpplap::Parse<Flags>(2, operand);
        const auto misuse = cpplap::Parse<Cli>(1, nullptr);
        const auto help = cpplap::HelpText<Cli>();
        parsed = std::unexpected(
            cpplap::ParseError{cpplap::ErrorCode::InvalidArgv, 0, {}});
        const auto count = cpplap_end_allocation_check();

        THEN("nothing was allocated and every path did real work") {
          CHECK(count == 0);
          CHECK(recursive);
          CHECK(paths == args.size() - 2);
          REQUIRE_FALSE(error);
          CHECK(error.error().token.size() == long_option.size() - 1);
          REQUIRE_FALSE(rejected);
          CHECK(rejected.error().code ==
                cpplap::ErrorCode::UnexpectedPositional);
          REQUIRE_FALSE(misuse);
          CHECK(misuse.error().code == cpplap::ErrorCode::InvalidArgv);
          CHECK_FALSE(help.empty());
        }
      }
    }
  }
}
