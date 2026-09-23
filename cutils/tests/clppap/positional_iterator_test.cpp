// See LICENSE in the repository root.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cutils/clppap/positional_iterator.hpp>
#include <iterator>
#include <span>

namespace {
constexpr cpplap::Option kOptions[]{{"-r", "--recursive", ""},
                                    {"-v", "--verbose", ""}};
}

SCENARIO("Positional iteration yields operands in order", "[clppap]") {
  GIVEN("flags interleaved with operands and a separator") {
    const char* const args[]{"first", "-r", "second", "--verbose", "--",
                             "-r",    "",   "-",      "--"};
    WHEN("the positionals are iterated") {
      const cpplap::PositionalIterator positionals{args, kOptions};
      THEN("flags and the first separator are skipped, later words are kept") {
        REQUIRE(std::ranges::equal(
            positionals,
            std::array{args[0], args[2], args[5], args[6], args[7], args[8]}));
      }
    }
  }
}

SCENARIO("Advancing an exhausted positional iterator is a no-op", "[clppap]") {
  GIVEN("an iterator whose last operand is followed only by a flag") {
    const char* const args[]{"only", "-v"};
    cpplap::PositionalIterator it{args, kOptions};
    REQUIRE(*it == args[0]);
    WHEN("it is advanced past the end repeatedly") {
      ++it;
      REQUIRE(it == std::default_sentinel);
      ++it;
      it++;
      THEN("it stays at the end") { REQUIRE(it == std::default_sentinel); }
    }
  }
  GIVEN("an iterator over no arguments") {
    const cpplap::PositionalIterator it{{}, {}};
    THEN("it starts at the end") { REQUIRE(it == std::default_sentinel); }
  }
}
