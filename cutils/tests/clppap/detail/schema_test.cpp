// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <cutils/clppap/clppap.hpp>
#include <cutils/clppap/detail/schema.hpp>

namespace {
using cpplap::SchemaError;

template <typename T>
consteval auto Rejects(SchemaError expected) -> bool {
  const auto schema = cpplap::detail::BuildSchema<T>();
  return !schema && schema.error() == expected;
}

struct Empty {};

struct[[= cpplap::Help("a tool")]] Kitchen {
  [[= cpplap::Short("-s")]] bool s = false;
  [[= cpplap::Positional]] cpplap::PositionalIterator paths;
  [[= cpplap::Long("--long")]] bool l = false;
  [[ = cpplap::Short("-h"), = cpplap::HelpFlag ]] bool help = false;
};

struct DuplicateShort {
  [[= cpplap::Short("-r")]] bool a;
  [[= cpplap::Short("-r")]] bool b;
};
struct DuplicateLong {
  [[= cpplap::Long("--recursive")]] bool a;
  [[= cpplap::Long("--recursive")]] bool b;
};
struct BadLong {
  [[= cpplap::Long("recursive")]] bool a;
};
struct BadShort {
  [[= cpplap::Short("-rr")]] bool a;
};
struct DuplicateText {
  [[ = cpplap::Short("-r"), = cpplap::Short("-v") ]] bool a;
};
struct EmbeddedNull {
  [[= cpplap::Long("--a\0b")]] bool a;
};
struct TwoCollectors {
  [[= cpplap::Positional]] cpplap::PositionalIterator a;
  [[= cpplap::Positional]] cpplap::PositionalIterator b;
};
struct DuplicatePositional {
  [[ = cpplap::Positional, = cpplap::Positional ]] cpplap::PositionalIterator a;
};
struct UnannotatedCollector {
  cpplap::PositionalIterator a;
};
struct PositionalFlag {
  [[= cpplap::Positional]] bool a;
};
struct UnnamedFlag {
  bool a;
};
struct UnsupportedField {
  [[= cpplap::Long("--count")]] int a;
};
struct[[= cpplap::Short("-r")]] SpelledType {};
struct[[= cpplap::HelpFlag]] HelpType {};
struct Tag {};
struct[[= Tag{}]] ForeignAnnotation {};
struct DoubleHelpAnnotation {
  [[ = cpplap::Short("-h"), = cpplap::HelpFlag, = cpplap::HelpFlag ]] bool h;
};
struct TwoHelpFlags {
  [[ = cpplap::Short("-h"), = cpplap::HelpFlag ]] bool h;
  [[ = cpplap::Short("-i"), = cpplap::HelpFlag ]] bool i;
};
struct HelpCollector {
  [[ = cpplap::Positional, = cpplap::HelpFlag ]] cpplap::PositionalIterator a;
};
struct UnspelledHelp {
  [[= cpplap::HelpFlag]] bool h;
};
}

SCENARIO("Short spellings are a dash and one printable non-dash character",
         "[clppap][schema]") {
  GIVEN("candidate short spellings") {
    THEN("single printable characters are accepted") {
      STATIC_REQUIRE(cpplap::detail::ValidShort("-a"));
      STATIC_REQUIRE(cpplap::detail::ValidShort("-1"));
      STATIC_REQUIRE(cpplap::detail::ValidShort("-~"));
    }
    THEN("separators, bundles, blanks and control characters are rejected") {
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("--"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("-="));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("-"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("-ab"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("- "));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("a"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort("-\x7f"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidShort(""));
    }
  }
}

SCENARIO("Long spellings are two dashes and an identifier-like name",
         "[clppap][schema]") {
  GIVEN("candidate long spellings") {
    THEN("letters, digits, dashes and underscores are accepted") {
      STATIC_REQUIRE(cpplap::detail::ValidLong("--v"));
      STATIC_REQUIRE(cpplap::detail::ValidLong("--foo-bar_1"));
      STATIC_REQUIRE(cpplap::detail::ValidLong("--_x"));
    }
    THEN("bare, triple-dash, spaced, valued and short forms are rejected") {
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong("--"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong("---x"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong("--fo o"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong("--foo="));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong("-x"));
      STATIC_REQUIRE_FALSE(cpplap::detail::ValidLong(""));
    }
  }
}

SCENARIO("A valid CLI type builds a schema in declaration order",
         "[clppap][schema]") {
  GIVEN("a described type mixing flags, a collector and a help flag") {
    constexpr auto schema = cpplap::detail::BuildSchema<Kitchen>();
    THEN("options skip the collector and keep their spelling axes") {
      STATIC_REQUIRE(schema);
      STATIC_REQUIRE(schema->description == "a tool");
      STATIC_REQUIRE(schema->options.size() == 3U);
      STATIC_REQUIRE(schema->options[0].short_name == "-s");
      STATIC_REQUIRE(schema->options[0].long_name.empty());
      STATIC_REQUIRE(schema->options[1].short_name.empty());
      STATIC_REQUIRE(schema->options[1].long_name == "--long");
    }
    THEN("the help index counts flags only") {
      STATIC_REQUIRE(schema->has_help);
      STATIC_REQUIRE(schema->help_index == 2U);
    }
    THEN("the collector is named after its member") {
      STATIC_REQUIRE(schema->has_positionals);
      STATIC_REQUIRE(schema->positional_name == "paths");
      STATIC_REQUIRE(schema->positional_help.empty());
    }
  }
  GIVEN("an empty type") {
    constexpr auto schema = cpplap::detail::BuildSchema<Empty>();
    THEN("it has no options, operands, help or description") {
      STATIC_REQUIRE(schema);
      STATIC_REQUIRE(schema->options.size() == 0U);
      STATIC_REQUIRE_FALSE(schema->has_positionals);
      STATIC_REQUIRE_FALSE(schema->has_help);
      STATIC_REQUIRE(schema->description.empty());
    }
  }
}

SCENARIO("Malformed CLI types are rejected with a specific error",
         "[clppap][schema]") {
  GIVEN("types with clashing or malformed spellings") {
    THEN("each is rejected for its spelling fault") {
      STATIC_REQUIRE(
          Rejects<DuplicateShort>(SchemaError::DuplicateOptionSpelling));
      STATIC_REQUIRE(
          Rejects<DuplicateLong>(SchemaError::DuplicateOptionSpelling));
      STATIC_REQUIRE(Rejects<BadLong>(SchemaError::InvalidOptionSpelling));
      STATIC_REQUIRE(Rejects<BadShort>(SchemaError::InvalidOptionSpelling));
      STATIC_REQUIRE(Rejects<DuplicateText>(SchemaError::DuplicateAnnotation));
      STATIC_REQUIRE(
          Rejects<EmbeddedNull>(SchemaError::EmbeddedNullAnnotation));
    }
  }
  GIVEN("types misusing the positional collector") {
    THEN("each is rejected for its collector fault") {
      STATIC_REQUIRE(
          Rejects<TwoCollectors>(SchemaError::InvalidPositionalAnnotations));
      STATIC_REQUIRE(Rejects<DuplicatePositional>(
          SchemaError::DuplicatePositionalAnnotation));
      STATIC_REQUIRE(Rejects<UnannotatedCollector>(
          SchemaError::InvalidPositionalAnnotations));
      STATIC_REQUIRE(
          Rejects<PositionalFlag>(SchemaError::InvalidFlagAnnotations));
    }
  }
  GIVEN("types with unnamed flags or unsupported members") {
    THEN("each is rejected") {
      STATIC_REQUIRE(Rejects<UnnamedFlag>(SchemaError::InvalidFlagAnnotations));
      STATIC_REQUIRE(
          Rejects<UnsupportedField>(SchemaError::UnsupportedFieldType));
    }
  }
  GIVEN("types carrying annotations that do not belong on a type") {
    THEN("each is rejected") {
      STATIC_REQUIRE(Rejects<SpelledType>(SchemaError::InvalidTypeAnnotation));
      STATIC_REQUIRE(Rejects<HelpType>(SchemaError::InvalidTypeAnnotation));
      STATIC_REQUIRE(
          Rejects<ForeignAnnotation>(SchemaError::UnsupportedAnnotation));
    }
  }
  GIVEN("types misusing the help flag") {
    THEN("each is rejected for its help fault") {
      STATIC_REQUIRE(
          Rejects<DoubleHelpAnnotation>(SchemaError::DuplicateHelpAnnotation));
      STATIC_REQUIRE(
          Rejects<TwoHelpFlags>(SchemaError::DuplicateHelpAnnotation));
      STATIC_REQUIRE(
          Rejects<HelpCollector>(SchemaError::InvalidHelpAnnotations));
      STATIC_REQUIRE(
          Rejects<UnspelledHelp>(SchemaError::InvalidFlagAnnotations));
    }
  }
}
