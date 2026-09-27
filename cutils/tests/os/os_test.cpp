// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <cutils/os/os.hpp>

TEST_CASE("getprogname is the basename of the test executable",
          "[os][getprogname]") {
  CHECK(cutils::os::getprogname() == "os_test");
}
