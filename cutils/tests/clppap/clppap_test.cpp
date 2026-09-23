// See LICENSE in the repository root.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cutils/clppap/clppap.hpp>
#include <iterator>
#include <utility>

namespace {
struct[[= cpplap::Help("rm alternative")]] Cli {
  [[
    = cpplap::Short("-r"), = cpplap::Long("--recursive"),
    = cpplap::Help("Remove directories")
  ]] bool recursive = false;
  [[ = cpplap::Positional,
     = cpplap::Help("Paths to remove") ]] cpplap::PositionalIterator positional;
  [[= cpplap::Long("--verbose")]] bool verbose = false;
};
struct FlagsOnly {
  [[= cpplap::Short("-q")]] bool quiet = true;
};
struct Empty {};
struct Uninitialized {
  [[= cpplap::Long("--flag")]] bool flag;
};
struct WithHelp {
  [[ = cpplap::Short("-h"), = cpplap::HelpFlag ]] bool help = false;
  [[= cpplap::Short("-r")]] bool recursive = false;
};
struct[[= cpplap::Help("just a description")]] DescOnly {};
struct PositionalOnlyHelp {
  [[ = cpplap::Positional,
     = cpplap::Help("the items") ]] cpplap::PositionalIterator items;
};
struct NoPosHelp {
  [[= cpplap::Short("-a")]] bool a = false;
  [[= cpplap::Positional]] cpplap::PositionalIterator paths;
};
struct[[= cpplap::Help("desc")]] DescPosOnly {
  [[= cpplap::Positional]] cpplap::PositionalIterator items;
};

struct SpellingRow {
  const char* spelling;
  bool recursive;
  bool verbose;
};
}

SCENARIO("Parsed flags land in their members around the collector",
         "[clppap][parse]") {
  GIVEN("a spelling given before and after an operand") {
    const auto row = GENERATE(SpellingRow{"-r", true, false},
                              SpellingRow{"--recursive", true, false},
                              SpellingRow{"--verbose", false, true});
    CAPTURE(row.spelling);
    const char* args[]{"prog", row.spelling, "file", row.spelling};
    WHEN("it is parsed") {
      const auto parsed = cpplap::Parse<Cli>(4, args);
      THEN("only its member is set and the operand is collected") {
        REQUIRE(parsed);
        CHECK(parsed->recursive == row.recursive);
        CHECK(parsed->verbose == row.verbose);
        CHECK(std::ranges::equal(parsed->positional, std::array{args[2]}));
      }
    }
  }
}

SCENARIO("Flags that are not given keep their declared defaults",
         "[clppap][parse]") {
  GIVEN("no arguments at all") {
    THEN("false defaults stay false and the collector is empty") {
      const auto parsed = cpplap::Parse<Cli>(0, nullptr);
      REQUIRE(parsed);
      CHECK_FALSE(parsed->recursive);
      CHECK_FALSE(parsed->verbose);
      CHECK(parsed->positional == std::default_sentinel);
    }
    THEN("a true default stays true") {
      const auto parsed = cpplap::Parse<FlagsOnly>(0, nullptr);
      REQUIRE(parsed);
      CHECK(parsed->quiet);
    }
    THEN("an uninitialized flag is value-initialized to false") {
      const auto parsed = cpplap::Parse<Uninitialized>(0, nullptr);
      REQUIRE(parsed);
      CHECK_FALSE(parsed->flag);
    }
    THEN("a type without members parses") {
      REQUIRE(cpplap::Parse<Empty>(0, nullptr));
    }
  }
}

SCENARIO("Scan failures surface unchanged from Parse", "[clppap][parse]") {
  GIVEN("an unknown option after an operand") {
    const char* args[]{"prog", "file", "--unknown"};
    WHEN("it is parsed") {
      const auto parsed = cpplap::Parse<Cli>(3, args);
      THEN("the error names and borrows the unknown word") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == cpplap::ErrorCode::UnknownOption);
        CHECK(parsed.error().argument_index == 2);
        CHECK(parsed.error().token.data() == args[2]);
      }
    }
  }
  GIVEN("an operand for a type without a collector") {
    const char* args[]{"cmd", "file"};
    WHEN("it is parsed") {
      const auto parsed = cpplap::Parse<FlagsOnly>(2, args);
      THEN("the operand is rejected") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == cpplap::ErrorCode::UnexpectedPositional);
        CHECK(parsed.error().argument_index == 1);
      }
    }
  }
}

SCENARIO("Parse treats the help flag as an ordinary flag", "[clppap][parse]") {
  GIVEN("a help flag alongside another flag") {
    const char* args[]{"prog", "-h", "-r"};
    WHEN("both are parsed") {
      const auto parsed = cpplap::Parse<WithHelp>(3, args);
      THEN("both members are set without short-circuiting") {
        REQUIRE(parsed);
        CHECK(parsed->help);
        CHECK(parsed->recursive);
      }
    }
  }
}

SCENARIO("Collected operands outlive the parse result", "[clppap][parse]") {
  GIVEN("a parsed result with operands around flags and a separator") {
    const char* args[]{"remmy",     "one", "-r",    "two",
                       "--verbose", "--",  "-three"};
    auto parsed = cpplap::Parse<Cli>(7, args);
    REQUIRE(parsed);
    WHEN("the value is moved out and the result is overwritten") {
      auto cli = std::move(*parsed);
      parsed = std::unexpected(
          cpplap::ParseError{cpplap::ErrorCode::InvalidArgv, 0, {}});
      THEN("the operands still iterate over the original argv") {
        CHECK(cli.recursive);
        CHECK(cli.verbose);
        CHECK(std::ranges::equal(cli.positional,
                                 std::array{args[1], args[3], args[6]}));
      }
    }
  }
}

SCENARIO("Help text renders only the sections a type declares",
         "[clppap][help]") {
  GIVEN("types with every combination of description, options and operands") {
    THEN("each section appears only when declared, separated by blank lines") {
      CHECK(cpplap::HelpText<Cli>() ==
            "rm alternative\n\nOptions:\n  -r, --recursive\n"
            "    Remove directories\n  --verbose\n\nArguments:\n"
            "  positional\n    Paths to remove\n");
      CHECK(cpplap::HelpText<DescOnly>() == "just a description\n");
      CHECK(cpplap::HelpText<PositionalOnlyHelp>() ==
            "Arguments:\n  items\n    the items\n");
      CHECK(cpplap::HelpText<FlagsOnly>() == "Options:\n  -q\n");
      CHECK(cpplap::HelpText<NoPosHelp>() ==
            "Options:\n  -a\n\nArguments:\n  paths\n");
      CHECK(cpplap::HelpText<DescPosOnly>() == "desc\n\nArguments:\n  items\n");
      CHECK(cpplap::HelpText<Empty>().empty());
    }
  }
}
