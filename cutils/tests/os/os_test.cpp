// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <cutils/os/os.hpp>
#include <thread>

TEST_CASE("PerformanceCores is between 1 and every CPU", "[os]") {
  const unsigned cores = cutils::os::PerformanceCores();
  CHECK(cores >= 1);
  if (const unsigned all = std::thread::hardware_concurrency(); all != 0) {
    CHECK(cores <= all);
  }
}
