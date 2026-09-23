// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <cstdlib>
#include <cutils/os/env.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {
using cutils::EnvError;
using cutils::EnvParser;
using cutils::GetEnv;
using cutils::GetEnvOrDefault;
constexpr const char* kName = "CUTILS_ENV_TEST";

class Environment {
 public:
  Environment() {
    if (const char* value = ::getenv(kName)) {
      previous_ = value;
    }
  }
  Environment(const Environment&) = delete;
  auto operator=(const Environment&) -> Environment& = delete;
  ~Environment() {
    if (previous_) {
      CHECK(::setenv(kName, previous_->c_str(), 1) == 0);
    } else {
      CHECK(::unsetenv(kName) == 0);
    }
  }
  static void Set(const char* value) {
    REQUIRE(::setenv(kName, value, 1) == 0);
  }
  static void Unset() { REQUIRE(::unsetenv(kName) == 0); }

 private:
  std::optional<std::string> previous_;
};

struct Custom {
  int value;
};
struct IntegralRow {
  const char* text;
  std::expected<std::uint16_t, EnvError> expected;
};
struct BooleanRow {
  const char* text;
  bool value;
};
}

template <>
struct cutils::EnvParser<Custom> {
  [[nodiscard]] static constexpr auto Parse(std::string_view text) noexcept
      -> std::expected<Custom, EnvError> {
    if (text != "custom") {
      return std::unexpected(EnvError::Malformed);
    }
    return Custom{42};
  }
};

SCENARIO("Missing environment values report an error or use a fallback",
         "[env]") {
  GIVEN("an unset environment variable") {
    const Environment restore;
    Environment::Unset();
    WHEN("it is read as an integer, boolean or string") {
      const auto missing = GetEnv<std::uint16_t>(kName);
      const auto boolean = GetEnv<bool>(kName);
      const auto text = GetEnv<std::string_view>(kName);
      THEN("each read reports that the variable is not set") {
        REQUIRE_FALSE(missing);
        CHECK(missing.error() == EnvError::NotSet);
        REQUIRE_FALSE(boolean);
        REQUIRE_FALSE(text);
        CHECK(boolean.error() == EnvError::NotSet);
        CHECK(text.error() == EnvError::NotSet);
      }
    }
    WHEN("a default value is supplied") {
      THEN("the default is returned") {
        CHECK(GetEnvOrDefault<std::uint16_t>(kName, 7) == 7);
      }
    }
  }
}

SCENARIO("Unsigned environment integers enforce syntax and range", "[env]") {
  GIVEN("an environment value at a syntax or range boundary") {
    const auto row = GENERATE(
        IntegralRow{"0", 0}, IntegralRow{"4", 4}, IntegralRow{"65535", 65535},
        IntegralRow{"65536", std::unexpected(EnvError::OutOfRange)},
        IntegralRow{"99999", std::unexpected(EnvError::OutOfRange)},
        IntegralRow{"-5", std::unexpected(EnvError::Malformed)},
        IntegralRow{"", std::unexpected(EnvError::Malformed)},
        IntegralRow{" 4", std::unexpected(EnvError::Malformed)},
        IntegralRow{"4x", std::unexpected(EnvError::Malformed)},
        IntegralRow{"4 ", std::unexpected(EnvError::Malformed)},
        IntegralRow{"0x10", std::unexpected(EnvError::Malformed)},
        IntegralRow{"+4", std::unexpected(EnvError::Malformed)},
        IntegralRow{"eight", std::unexpected(EnvError::Malformed)});
    CAPTURE(row.text);
    const Environment restore;
    Environment::Set(row.text);
    WHEN("it is read as a 16-bit unsigned integer") {
      const auto parsed = GetEnv<std::uint16_t>(kName);
      THEN("it returns the expected value or parsing error") {
        REQUIRE(parsed.has_value() == row.expected.has_value());
        if (row.expected) {
          CHECK(*parsed == *row.expected);
        } else {
          CHECK(parsed.error() == row.expected.error());
        }
      }
      AND_THEN("a supplied default is used only when parsing fails") {
        CHECK(GetEnvOrDefault<std::uint16_t>(kName, 7) ==
              row.expected.value_or(7));
      }
    }
  }
}

SCENARIO("Signed environment integers accept positive and negative values",
         "[env]") {
  GIVEN("an environment variable containing a signed decimal integer") {
    const auto row = GENERATE(std::pair{"4", 4}, std::pair{"-5", -5},
                              std::pair{"99999", 99999});
    CAPTURE(row.first);
    const Environment restore;
    Environment::Set(row.first);
    WHEN("it is read as a 32-bit signed integer") {
      const auto parsed = GetEnv<std::int32_t>(kName);
      THEN("the integer value is returned") {
        REQUIRE(parsed);
        CHECK(*parsed == row.second);
      }
    }
  }
}

SCENARIO("Environment booleans accept supported spellings", "[env]") {
  GIVEN("an environment variable containing a supported boolean spelling") {
    const auto row =
        GENERATE(BooleanRow{"1", true}, BooleanRow{"true", true},
                 BooleanRow{"TRUE", true}, BooleanRow{"True", true},
                 BooleanRow{"yes", true}, BooleanRow{"YES", true},
                 BooleanRow{"on", true}, BooleanRow{"0", false},
                 BooleanRow{"false", false}, BooleanRow{"FALSE", false},
                 BooleanRow{"No", false}, BooleanRow{"off", false},
                 BooleanRow{"OFF", false});
    CAPTURE(row.text);
    const Environment restore;
    Environment::Set(row.text);
    WHEN("it is read as a boolean") {
      const auto parsed = GetEnv<bool>(kName);
      THEN("the corresponding boolean value is returned") {
        REQUIRE(parsed);
        CHECK(*parsed == row.value);
      }
    }
  }
}

SCENARIO("Malformed environment booleans report an error or use a fallback",
         "[env]") {
  GIVEN("an environment variable containing an invalid boolean spelling") {
    const auto* text = GENERATE("", "maybe", "2", "tru", "yes ");
    CAPTURE(text);
    const Environment restore;
    Environment::Set(text);
    WHEN("it is read as a boolean") {
      const auto parsed = GetEnv<bool>(kName);
      THEN("parsing reports a malformed value") {
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error() == EnvError::Malformed);
      }
      AND_THEN("a supplied default is returned") {
        CHECK(GetEnvOrDefault(kName, true));
      }
    }
  }
}

SCENARIO("Environment string views borrow storage, including empty values",
         "[env]") {
  GIVEN("a set environment variable") {
    const auto* text = GENERATE("hello world", "");
    CAPTURE(text);
    const Environment restore;
    Environment::Set(text);
    WHEN("it is read as a string view") {
      const auto parsed = GetEnv<std::string_view>(kName);
      THEN("the view refers directly to the environment storage") {
        REQUIRE(parsed);
        CHECK(*parsed == text);
        CHECK(parsed->data() == ::getenv(kName));
      }
      AND_THEN("the value is returned instead of a supplied default") {
        CHECK(GetEnvOrDefault<std::string_view>(kName, "fallback") == text);
      }
    }
  }
}

SCENARIO("Environment values support custom parsers and fallbacks", "[env]") {
  GIVEN("an environment value accepted by a custom parser") {
    const Environment restore;
    Environment::Set("custom");
    WHEN("it is read using the custom parser") {
      const auto parsed = GetEnv<Custom>(kName);
      THEN("the custom value is returned") {
        REQUIRE(parsed);
        CHECK(parsed->value == 42);
      }
      AND_WHEN("the environment changes to a value the parser rejects") {
        Environment::Set("other");
        THEN("a supplied custom default is returned") {
          CHECK(GetEnvOrDefault(kName, Custom{1}).value == 1);
        }
      }
    }
  }
}
