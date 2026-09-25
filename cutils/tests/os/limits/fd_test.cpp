// See LICENSE in the repository root.

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <cutils/os/os.hpp>
#include <optional>
#include <system_error>
#include <vector>

namespace {
namespace limits = cutils::os::limits::fd;
using cutils::os::Fd;
using limits::Pool;

auto RawLimits() -> ::rlimit {
  ::rlimit current{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &current) == 0);
  return current;
}

auto Cap() -> std::uint64_t {
  int value = 0;
  std::size_t size = sizeof(value);
  REQUIRE(::sysctlbyname("kern.maxfilesperproc", &value, &size, nullptr, 0) ==
          0);
  REQUIRE(value > 0);
  return static_cast<std::uint64_t>(value);
}

void RequireApplied(std::uint64_t expected) {
  REQUIRE(RawLimits().rlim_cur == expected);
  const auto reported = limits::Get();
  REQUIRE(reported);
  REQUIRE(*reported == expected);
}

class RestoreSoft {
 public:
  RestoreSoft() : original_(RawLimits()) {}
  RestoreSoft(const RestoreSoft&) = delete;
  auto operator=(const RestoreSoft&) -> RestoreSoft& = delete;
  ~RestoreSoft() { CHECK(::setrlimit(RLIMIT_NOFILE, &original_) == 0); }

 private:
  ::rlimit original_;
};

auto LowerHard() -> std::uint64_t {
  auto limit = RawLimits();
  limit.rlim_max =
      std::min({limit.rlim_max, static_cast<rlim_t>(Cap()), rlim_t{64}});
  limit.rlim_cur = limit.rlim_max;
  REQUIRE(limit.rlim_max > 0);
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &limit) == 0);
  return limit.rlim_max;
}

SCENARIO("Get reports the effective descriptor capacity", "[limits]") {
  GIVEN("the inherited soft limit and per-process cap") {
    const auto expected = std::min<std::uint64_t>(RawLimits().rlim_cur, Cap());
    WHEN("the limit is queried") {
      const auto reported = limits::Get();
      THEN("it is the smaller of the soft limit and the cap") {
        REQUIRE(reported);
        REQUIRE(*reported > 0);
        REQUIRE(*reported == expected);
      }
    }
  }
}

SCENARIO("Setting the soft limit round trips within the inherited hard limit",
         "[limits]") {
  GIVEN("a target within the hard limit and per-process cap") {
    const RestoreSoft restore;
    const auto target =
        std::min({std::uint64_t{1024},
                  static_cast<std::uint64_t>(RawLimits().rlim_max), Cap()});
    REQUIRE(target >= 2);
    const auto lower = target / 2;

    WHEN("the soft limit is lowered to half the target") {
      REQUIRE(limits::Set(lower));
      THEN("the lower limit is applied") { RequireApplied(lower); }

      AND_WHEN("it is raised back to the target") {
        REQUIRE(limits::Set(target));
        THEN("the target limit is applied") { RequireApplied(target); }
      }
    }
  }
}

SCENARIO("SetMax raises the soft limit to the highest permitted value",
         "[limits]") {
  GIVEN("the inherited hard limit and per-process cap") {
    const RestoreSoft restore;
    const auto expected = std::min<std::uint64_t>(RawLimits().rlim_max, Cap());
    WHEN("the soft limit is maximised") {
      const auto target = limits::SetMax();
      THEN("it reports and applies the smaller of the hard limit and the cap") {
        REQUIRE(target);
        REQUIRE(*target == expected);
        REQUIRE(*target < static_cast<std::uint64_t>(RLIM_INFINITY));
        RequireApplied(*target);
      }
    }
  }
}

SCENARIO("Raising the soft limit above the hard limit is refused", "[limits]") {
  GIVEN("a lowered hard limit") {
    const auto hard = LowerHard();
    WHEN("the soft limit is set one past it") {
      const auto raised = limits::Set(hard + 1);
      THEN("it fails with EINVAL and the soft limit is unchanged") {
        REQUIRE_FALSE(raised);
        REQUIRE(raised.error() == std::errc::invalid_argument);
        RequireApplied(hard);
      }
    }
  }
}

SCENARIO("A refused open says whether waiting on our descriptors can help",
         "[limits]") {
  GIVEN("a lowered hard limit") {
    const auto hard = LowerHard();
    THEN("capacity matches the hard limit and the pool is not exhausted") {
      REQUIRE(Pool::Capacity() == hard);
      REQUIRE_FALSE(Pool::Exhausted());
    }

    // Observe everything before asserting: while no descriptor is free,
    // sanitizer runtimes (which probe memory through a pipe) and Catch's
    // reporting cannot run, so the descriptors are released first.
    WHEN("the limit is filled by descriptors the pool does not own") {
      std::vector<int> foreign;
      for (;;) {
        const int raw = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (raw < 0) {
          break;
        }
        foreign.push_back(raw);
      }
      const auto refused = cutils::os::open("/dev/null", O_RDONLY | O_CLOEXEC);
      const bool exhausted = Pool::Exhausted();
      for (const int raw : foreign) {
        ::close(raw);
      }

      THEN("the refusal is EMFILE and not retryable") {
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().code == std::errc::too_many_files_open);
        REQUIRE_FALSE(refused.error().retryable);
      }
      THEN("the pool learns no ceiling from it") { REQUIRE_FALSE(exhausted); }
    }
    WHEN("the limit is filled by descriptors the pool owns") {
      std::vector<Fd> held;
      std::optional<cutils::os::OpenError> refusal;
      for (;;) {
        auto opened = cutils::os::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (!opened) {
          refusal = opened.error();
          break;
        }
        held.push_back(*std::move(opened));
      }
      const bool exhausted_when_full = Pool::Exhausted();
      if (!held.empty()) {
        held.pop_back();
      }
      const bool exhausted_after_one = Pool::Exhausted();
      held.clear();

      THEN("the refusal is EMFILE and retryable") {
        REQUIRE(refusal);
        REQUIRE(refusal->code == std::errc::too_many_files_open);
        REQUIRE(refusal->retryable);
      }
      THEN("the refusal marks the pool exhausted") {
        REQUIRE(exhausted_when_full);
      }
      THEN("releasing one descriptor ends the exhaustion") {
        REQUIRE_FALSE(exhausted_after_one);
      }
    }
  }
}

SCENARIO("An open failing for another reason is never retryable", "[limits]") {
  GIVEN("a path that does not exist") {
    WHEN("it is opened") {
      const auto missing =
          cutils::os::open("/nonexistent/cutils", O_RDONLY | O_CLOEXEC);
      THEN("the error is ENOENT and not retryable") {
        REQUIRE_FALSE(missing);
        REQUIRE(missing.error().code == std::errc::no_such_file_or_directory);
        REQUIRE_FALSE(missing.error().retryable);
      }
    }
  }
}
}  // namespace
