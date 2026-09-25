// See LICENSE in the repository root.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdio>
#include <cutils/clppap/report.hpp>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace {
struct[[= cpplap::Help("reported")]] Reported {
  [[= cpplap::Short("-r")]] bool recursive = false;
  [[= cpplap::Positional]] cpplap::PositionalIterator positional;
  [[
    = cpplap::Short("-h"), = cpplap::Long("--help"),
    = cpplap::Help("Show this help"), = cpplap::HelpFlag
  ]] bool help = false;
};
struct NoHelpFlags {
  [[= cpplap::Short("-q")]] bool quiet = true;
};
struct HelpFirst {
  [[ = cpplap::Short("-h"), = cpplap::HelpFlag ]] bool help = false;
  [[= cpplap::Short("-r")]] bool recursive = false;
};

// A functor, not decltype(&std::fclose): glibc's attributes on fclose would be
// dropped from the template argument, which GCC warns about.
struct CloseFile {
  void operator()(std::FILE* file) const noexcept { (void)std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, CloseFile>;
auto TempFile() -> File {
  File file{std::tmpfile()};
  REQUIRE(file != nullptr);
  return file;
}
auto Drain(std::FILE* stream) -> std::string {
  std::rewind(stream);
  std::string text;
  for (int c = std::fgetc(stream); c != EOF; c = std::fgetc(stream)) {
    text.push_back(static_cast<char>(c));
  }
  REQUIRE(std::ferror(stream) == 0);
  return text;
}

struct FormatRow {
  cpplap::ErrorCode code;
  std::size_t index;
  std::string_view token;
  std::string_view expected;
};
struct NameRow {
  const char* path;
  std::string_view expected;
};
struct RouteRow {
  const char* id;
  std::array<const char*, 4> args;
  int count;
  int exit;
  std::string_view diagnostic;
};
}

SCENARIO("Parse errors format as one-line diagnostics", "[clppap][report]") {
  using cpplap::ErrorCode;
  GIVEN("an error of each code") {
    const auto row = GENERATE(
        FormatRow{ErrorCode::NullArgument, 3, "ignored",
                  "argument 3 is a null pointer"},
        FormatRow{ErrorCode::InvalidArgv, 9, "ignored",
                  "malformed argument vector"},
        FormatRow{ErrorCode::UnknownOption, 2, "--nope",
                  "unknown option 2: '--nope'"},
        FormatRow{ErrorCode::UnknownOption, 0, "", "unknown option 0: ''"},
        FormatRow{ErrorCode::UnexpectedPositional, 1, "file",
                  "unexpected argument 1: 'file'"},
        FormatRow{static_cast<ErrorCode>(0xFFU), 5, "tok", ""});
    CAPTURE(row.code, row.index, row.token);
    WHEN("it is formatted") {
      const auto text =
          std::format("{}", cpplap::ParseError{row.code, row.index, row.token});
      THEN("the token is quoted only where it is meaningful") {
        CHECK(text == row.expected);
      }
    }
  }
}

SCENARIO("The program name is the basename of argv[0]", "[clppap][report]") {
  GIVEN("an argv[0] path") {
    const auto row =
        GENERATE(NameRow{"/a/b/c/tool", "tool"}, NameRow{"tool", "tool"},
                 NameRow{"./a/b/prog", "prog"}, NameRow{"/a/b/", ""},
                 NameRow{"/", ""}, NameRow{nullptr, ""});
    CAPTURE(row.path);
    const char* argv[]{row.path};
    THEN("everything after the last slash is used") {
      CHECK(cpplap::detail::ProgramName(1, argv) == row.expected);
    }
  }
  GIVEN("no usable argv") {
    const int count = GENERATE(-1, 0, 1);
    CAPTURE(count);
    THEN("the name is empty") {
      CHECK(cpplap::detail::ProgramName(count, nullptr).empty());
    }
  }
}

SCENARIO("ParseOrReport routes success, help and errors", "[clppap][report]") {
  GIVEN("a command line") {
    const auto row = GENERATE(
        RouteRow{
            "ordinary flag", {"/bin/remmy", "-r", "file", nullptr}, 3, -1, ""},
        RouteRow{"help after separator",
                 {"/bin/remmy", "--", "--help", nullptr},
                 3,
                 -1,
                 ""},
        RouteRow{
            "long help", {"/bin/remmy", "--help", nullptr, nullptr}, 2, 0, ""},
        RouteRow{
            "help with flag", {"/bin/remmy", "-r", "-h", nullptr}, 3, 0, ""},
        RouteRow{"help before error",
                 {"/bin/remmy", "--help", "--nope", nullptr},
                 3,
                 1,
                 "remmy: unknown option 2: '--nope'\n"},
        RouteRow{"null argument",
                 {"/bin/remmy", nullptr, "--nope", nullptr},
                 3,
                 1,
                 "remmy: argument 1 is a null pointer\n"},
        RouteRow{"missing program",
                 {nullptr, "--nope", nullptr, nullptr},
                 2,
                 1,
                 "malformed argument vector\n"});
    CAPTURE(row.id);
    auto out = TempFile();
    auto err = TempFile();
    WHEN("it is parsed and reported") {
      const auto parsed = cpplap::ParseOrReport<Reported>(
          row.count, row.args.data(), out.get(), err.get());
      THEN("the result, exit code and streams match the route") {
        if (row.exit < 0) {
          REQUIRE(parsed);
          CHECK_FALSE(parsed->help);
        } else {
          REQUIRE_FALSE(parsed);
          CHECK(parsed.error() == row.exit);
        }
        CHECK(Drain(out.get()) ==
              (row.exit == 0 ? cpplap::HelpText<Reported>() : ""));
        CHECK(Drain(err.get()) == row.diagnostic);
      }
    }
  }
}

SCENARIO("The help flag is found at any flag index", "[clppap][report]") {
  GIVEN("a type whose help flag is its first flag") {
    auto out = TempFile();
    auto err = TempFile();
    WHEN("only the other flag is given") {
      const char* args[]{"prog", "-r"};
      const auto parsed =
          cpplap::ParseOrReport<HelpFirst>(2, args, out.get(), err.get());
      THEN("it parses without printing help") {
        REQUIRE(parsed);
        CHECK(parsed->recursive);
        CHECK(Drain(out.get()).empty());
      }
    }
    WHEN("the help flag is given") {
      const char* args[]{"prog", "-h"};
      const auto parsed =
          cpplap::ParseOrReport<HelpFirst>(2, args, out.get(), err.get());
      THEN("help is printed with exit code zero") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error() == 0);
        CHECK(Drain(out.get()) == cpplap::HelpText<HelpFirst>());
        CHECK(Drain(err.get()).empty());
      }
    }
  }
}

SCENARIO("Types without a help flag never report help", "[clppap][report]") {
  GIVEN("a type whose only flag defaults to true") {
    auto out = TempFile();
    auto err = TempFile();
    WHEN("it is parsed with no arguments") {
      const auto parsed =
          cpplap::ParseOrReport<NoHelpFlags>(0, nullptr, out.get(), err.get());
      THEN("the true flag is not mistaken for a help request") {
        REQUIRE(parsed);
        CHECK(parsed->quiet);
        CHECK(Drain(out.get()).empty());
        CHECK(Drain(err.get()).empty());
      }
    }
  }
}
