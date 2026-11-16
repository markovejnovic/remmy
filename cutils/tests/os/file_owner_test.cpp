// See LICENSE in the repository root.

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <cutils/os/file_flags.hpp>
#include <cutils/os/owner_names.hpp>
#include <string>
#include <utility>

namespace {

// Far above any account a test machine has.
constexpr std::uint32_t kUnusedId = 3'999'999;

}  // namespace

TEST_CASE(
    "The current user's and group's names are what the passwd and group "
    "databases say",
    "[owner_names]") {
  const ::passwd* user = ::getpwuid(::getuid());
  const ::group* group = ::getgrgid(::getgid());
  REQUIRE(user != nullptr);
  REQUIRE(group != nullptr);
  CHECK(cutils::os::UserName(::getuid()) == user->pw_name);
  CHECK(cutils::os::GroupName(::getgid()) == group->gr_name);
}

TEST_CASE("An id with no name prints as its number", "[owner_names]") {
  CHECK(cutils::os::UserName(kUnusedId) == std::to_string(kUnusedId));
  CHECK(cutils::os::GroupName(kUnusedId) == std::to_string(kUnusedId));
}

#if defined(__APPLE__)
TEST_CASE("File flags print as rm and ls -lo name them", "[file_flags]") {
  const auto [flags, names] =
      GENERATE(std::pair<std::uint32_t, std::string>{0, ""},
               std::pair<std::uint32_t, std::string>{UF_IMMUTABLE, "uchg"},
               std::pair<std::uint32_t, std::string>{UF_IMMUTABLE | UF_APPEND,
                                                     "uappnd,uchg"},
               std::pair<std::uint32_t, std::string>{UF_HIDDEN, "hidden"},
               std::pair<std::uint32_t, std::string>{UF_NODUMP, "nodump"});
  CAPTURE(flags);
  CHECK(cutils::os::FlagNames(flags) == names);
}
#else
TEST_CASE("Linux has no file flags to name", "[file_flags]") {
  CHECK(cutils::os::FlagNames(0).empty());
}
#endif
