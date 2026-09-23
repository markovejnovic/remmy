// See LICENSE in the repository root.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cutils/clppap/detail/argument_set.hpp>
#include <iterator>

namespace {
constexpr std::array kOptions{
    cpplap::Option{"-r", "--recursive", ""},
    cpplap::Option{"-v", "--verbose", ""},
};
using Set = cpplap::detail::ArgumentSet<kOptions.size()>;
using cpplap::ErrorCode;
using cpplap::detail::PositionalPolicy;
constexpr auto Allow = PositionalPolicy::Allow;
constexpr auto Reject = PositionalPolicy::Reject;

struct FlagRow {
  const char* id;
  std::array<const char*, 4> args;
  bool first;
  bool second;
};

constexpr const char* kNullProgram[]{nullptr, "-r"};

struct ArgvRow {
  int count;
  const char* const* args;
};

struct ErrorRow {
  const char* id;
  std::array<const char*, 3> args;
  PositionalPolicy policy;
  ErrorCode code;
  std::size_t index;
};
}

SCENARIO("Scanning records which flags appeared", "[clppap][argument_set]") {
  GIVEN("flags repeated under both spellings and a trailing separator") {
    const auto row = GENERATE(
        FlagRow{"first", {"-r", "--recursive", "-r", "--"}, true, false},
        FlagRow{"second", {"-v", "--verbose", "-v", "--"}, false, true},
        FlagRow{"both", {"-r", "--verbose", "-r", "--"}, true, true});
    const auto policy = GENERATE(Allow, Reject);
    CAPTURE(row.id, policy);
    WHEN("they are scanned") {
      const auto set = Set::Scan(row.args, kOptions, policy);
      THEN("each flag is set exactly when one of its spellings appeared") {
        REQUIRE(set);
        CHECK(set->Flag(0) == row.first);
        CHECK(set->Flag(1) == row.second);
        CHECK(set->Positionals() == std::default_sentinel);
      }
    }
  }
}

SCENARIO("Scanning stops at the first failing argument",
         "[clppap][argument_set]") {
  GIVEN("arguments containing several faults") {
    const auto row = GENERATE(ErrorRow{"unknown before null",
                                       {"-r", "--unknown", nullptr},
                                       Allow,
                                       ErrorCode::UnknownOption,
                                       2},
                              ErrorRow{"null before unknown",
                                       {nullptr, "--unknown", "file"},
                                       Allow,
                                       ErrorCode::NullArgument,
                                       1},
                              ErrorRow{"null after separator",
                                       {"--", nullptr, "file"},
                                       Allow,
                                       ErrorCode::NullArgument,
                                       2},
                              ErrorRow{"operand before unknown",
                                       {"file", "--unknown", nullptr},
                                       Reject,
                                       ErrorCode::UnexpectedPositional,
                                       1},
                              ErrorRow{"operand after separator",
                                       {"-r", "--", "-r"},
                                       Reject,
                                       ErrorCode::UnexpectedPositional,
                                       3},
                              ErrorRow{"null under reject",
                                       {"--", nullptr, "file"},
                                       Reject,
                                       ErrorCode::NullArgument,
                                       2});
    CAPTURE(row.id);
    WHEN("they are scanned") {
      const auto parsed = Set::Scan(row.args, kOptions, row.policy);
      THEN("the error names the first fault and borrows its word") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == row.code);
        CHECK(parsed.error().argument_index == row.index);
        CHECK(parsed.error().token.data() == row.args[row.index - 1]);
      }
    }
  }
}

SCENARIO("argc bounds the scan and argv must name a program",
         "[clppap][argument_set]") {
  GIVEN("an argv whose trailing entries lie beyond argc") {
    const char* args[]{"remmy", "-r", "file", "--unknown"};
    const int count = GENERATE(0, 1, 2, 3);
    CAPTURE(count);
    WHEN("the first argc entries are scanned") {
      const auto parsed = Set::Scan(count, args, kOptions, Allow);
      THEN("the program name and anything past argc are ignored") {
        REQUIRE(parsed);
        CHECK(parsed->Flag(0) == (count >= 2));
        CHECK((parsed->Positionals() == std::default_sentinel) == (count < 3));
      }
    }
  }
  GIVEN("a malformed argument vector") {
    const auto row = GENERATE(ArgvRow{-1, nullptr}, ArgvRow{1, nullptr},
                              ArgvRow{2, kNullProgram});
    CAPTURE(row.count);
    WHEN("it is scanned") {
      const auto parsed = Set::Scan(row.count, row.args, kOptions, Allow);
      THEN("it is rejected as invalid argv with no token") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == ErrorCode::InvalidArgv);
        CHECK(parsed.error().argument_index == 0);
        CHECK(parsed.error().token.data() == nullptr);
      }
    }
  }
  GIVEN("argc of zero with a null argv") {
    THEN("there is nothing to validate and the scan succeeds") {
      REQUIRE(Set::Scan(0, nullptr, kOptions, Reject));
    }
  }
}
