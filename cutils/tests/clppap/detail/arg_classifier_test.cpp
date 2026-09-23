// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cutils/clppap/detail/arg_classifier.hpp>
#include <iterator>
#include <string_view>

namespace {
using cpplap::ErrorCode;
using cpplap::Option;
using cpplap::detail::ArgClassifier;
using cpplap::detail::TokenKind;
constexpr Option kOptions[]{{"-r", "--recursive", ""}, {"-v", "--verbose", ""}};

struct Row {
  const char* input;
  TokenKind kind;
  std::size_t option;
};
}

SCENARIO("Single words are classified lexically", "[clppap][arg_classifier]") {
  GIVEN("a word that is an operand, the separator or a known spelling") {
    const auto row = GENERATE(
        Row{"", TokenKind::Positional, 0}, Row{"-", TokenKind::Positional, 0},
        Row{"foo", TokenKind::Positional, 0},
        Row{"x--y", TokenKind::Positional, 0},
        Row{"--", TokenKind::Separator, 0}, Row{"-r", TokenKind::Flag, 0},
        Row{"--recursive", TokenKind::Flag, 0}, Row{"-v", TokenKind::Flag, 1},
        Row{"--verbose", TokenKind::Flag, 1});
    CAPTURE(row.input);
    const char* args[]{row.input};
    WHEN("it is classified") {
      const auto token = *ArgClassifier{args, kOptions};
      THEN("it has the expected kind, option and one-based index") {
        REQUIRE(token);
        CHECK(token->kind == row.kind);
        CHECK(token->option_index == row.option);
        CHECK(token->argument == row.input);
        CHECK(token->argument_index == 1);
      }
    }
  }
}

SCENARIO("Unrecognised dash words are unknown options",
         "[clppap][arg_classifier]") {
  GIVEN("a bundled, valued, miscased or unknown dash word") {
    const char* input = GENERATE("--unknown", "-rv", "--recursive=false", "-R");
    CAPTURE(input);
    const char* args[]{input};
    WHEN("it is classified") {
      const auto token = *ArgClassifier{args, kOptions};
      THEN("the error borrows the offending word") {
        REQUIRE_FALSE(token);
        CHECK(token.error().code == ErrorCode::UnknownOption);
        CHECK(token.error().argument_index == 1);
        CHECK(token.error().token.data() == input);
      }
    }
  }
}

SCENARIO("The first separator makes every later word an operand",
         "[clppap][arg_classifier]") {
  GIVEN("words after a separator, including a null argument") {
    const char* args[]{"-r", "--", "--", "-r", nullptr, "--nope", ""};
    WHEN("the words are classified in order") {
      ArgClassifier classifier{args, kOptions};
      REQUIRE((*classifier)->kind == TokenKind::Flag);
      ++classifier;
      REQUIRE((*classifier)->kind == TokenKind::Separator);
      ++classifier;
      THEN("dash words are operands and a null is still reported") {
        for (std::size_t i = 2; i < std::size(args); ++i) {
          CAPTURE(i);
          const auto token = *classifier;
          if (args[i] == nullptr) {
            REQUIRE_FALSE(token);
            CHECK(token.error().code == ErrorCode::NullArgument);
            CHECK(token.error().argument_index == i + 1);
            CHECK(token.error().token.data() == nullptr);
          } else {
            REQUIRE(token);
            CHECK(token->kind == TokenKind::Positional);
            CHECK(token->argument == args[i]);
            CHECK(token->argument_index == i + 1);
          }
          ++classifier;
        }
        REQUIRE(classifier == std::default_sentinel);
      }
    }
  }
}

SCENARIO("Empty spellings of long-only options never match",
         "[clppap][arg_classifier]") {
  GIVEN("an option table whose entries lack short or long spellings") {
    constexpr Option options[]{{"", "", ""}, {"", "--real", ""}};
    WHEN("the long spelling is classified") {
      const char* args[]{"--real"};
      const auto token = *ArgClassifier{args, options};
      THEN("it matches the entry that declares it") {
        REQUIRE(token);
        CHECK(token->kind == TokenKind::Flag);
        CHECK(token->option_index == 1);
      }
    }
    WHEN("a short dash word is classified") {
      const char* args[]{"-x"};
      const auto token = *ArgClassifier{args, options};
      THEN("no empty spelling matches it") {
        REQUIRE_FALSE(token);
        CHECK(token.error().code == ErrorCode::UnknownOption);
      }
    }
  }
}
