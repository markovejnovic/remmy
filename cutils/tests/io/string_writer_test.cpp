// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cutils/io/print.hpp>
#include <cutils/io/string_writer.hpp>
#include <cutils/io/warn.hpp>
#include <string>
#include <string_view>

SCENARIO("A string writer keeps everything written to it, in order",
         "[string_writer]") {
  GIVEN("a string writer over a string that already holds text") {
    std::string out = "kept ";
    cutils::io::StringWriter writer(out);

    WHEN("a line and a warning are written") {
      REQUIRE(cutils::io::PrintLn(writer, "{}/{}", std::string_view{"d"},
                                  std::string_view{"x"}));
      REQUIRE(cutils::io::Warn(writer, "rm", ENOENT, "{}",
                               std::string_view{"gone"}));

      THEN("the string holds the old text, then both, byte for byte") {
        CHECK(out == "kept d/x\nrm: gone: No such file or directory\n");
      }
    }
  }
}
